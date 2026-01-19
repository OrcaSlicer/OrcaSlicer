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
#include <algorithm>
#include <functional>
#include <iterator>
#include <numeric> // std::iota
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "mcut/internal/bvh.h"
#include "mcut/internal/hmesh.h"
#include "mcut/internal/kernel.h"
#include "mcut/internal/math.h"
#include "mcut/internal/timer.h"
#include "mcut/internal/utils.h"

#ifndef LICENSE_PURCHASED
#define lmsg() printf("NOTE: MCUT is copyrighted and may not be sold or included in commercial products without a license.\n")
#else
#define lmsg()
#endif // #ifndef LICENSE_PURCHASED

namespace std {
// need to declare partial and explicit specializations in every translation unit
// that uses them (before any use that would implicitly instantiate that
// specialization)
template <>
typename edge_array_iterator_t::difference_type distance(
    edge_array_iterator_t first,
    edge_array_iterator_t last);
}

logger_t* logger_ptr = nullptr;
std::string to_string(const sm_frag_location_t& v)
{
    std::string s;
    switch (v) {
    case sm_frag_location_t::ABOVE:
        s = "a";
        break;
    case sm_frag_location_t::BELOW:
        s = "b";
        break;
    case sm_frag_location_t::UNDEFINED:
        s = "u";
        break;
    }
    return s;
}

std::string to_string(const cm_patch_location_t& v)
{
    std::string s;
    switch (v) {
    case cm_patch_location_t::INSIDE:
        s = "i";
        break;
    case cm_patch_location_t::OUTSIDE:
        s = "o";
        break;
    case cm_patch_location_t::UNDEFINED:
        s = "u";
        break;
    }
    return s;
}

std::string to_string(const status_t& v)
{
    std::string s;
    switch (v) {
    case status_t::SUCCESS:
        s = "SUCCESS";
        break;
    case status_t::INVALID_SRC_MESH:
        s = "INVALID_SRC_MESH";
        break;
    case status_t::INVALID_CUT_MESH:
        s = "INVALID_CUT_MESH";
        break;
    case status_t::INVALID_MESH_INTERSECTION:
        s = "INVALID_MESH_INTERSECTION";
        break;
    case status_t::GENERAL_POSITION_VIOLATION:
        s = "GENERAL_POSITION_VIOLATION";
        break;
    case status_t::DETECTED_FLOATING_POLYGON:
        s = "DETECTED_FLOATING_POLYGON";
        break;
        // case status_t::FACE_VERTEX_INTERSECTION:
        //     s = "FACE_VERTEX_INTERSECTION";
        //     break;
    }
    return s;
}

std::string to_string(const cm_patch_winding_order_t& v)
{
    std::string s;
    switch (v) {
    case cm_patch_winding_order_t::DEFAULT:
        s = "def";
        break;
    case cm_patch_winding_order_t::REVERSE:
        s = "rev";
        break;
    }
    return s;
}

// returns whether a polygon-soup vertex is an intersection vertex/point
inline bool m0_is_intersection_point(const vd_t& ps_vd, const int ps_vtx_cnt)
{
    return ((int)ps_vd) >= ps_vtx_cnt;
}

// returns whether a polygon-soup vertex belongs to the cut mesh
bool inline ps_is_cutmesh_vertex(const vd_t& ps_vd, const int sm_vtx_cnt)
{
    return ((int)ps_vd) >= sm_vtx_cnt;
}

bool inline ps_is_cutmesh_face(const fd_t& ps_fd, const int sm_face_count)
{
    return ((int)ps_fd) >= sm_face_count;
}

void dump_mesh(const hmesh_t& mesh, const char* fbasename)
{
    const std::string name = std::string(fbasename) + ".off";

    for (vertex_array_iterator_t v = mesh.vertices_begin(); v != mesh.vertices_end(); ++v) {
    }

    for (edge_array_iterator_t e = mesh.edges_begin(); e != mesh.edges_end(); ++e) {
    }

    for (halfedge_array_iterator_t h = mesh.halfedges_begin(); h != mesh.halfedges_end(); ++h) {
    }

    for (face_array_iterator_t face_iter = mesh.faces_begin(); face_iter != mesh.faces_end(); ++face_iter) {

        const std::vector<halfedge_descriptor_t>& halfedges_around_face = mesh.get_halfedges_around_face(*face_iter);

        // int num_halfedges = ;
        MCUT_ASSERT((int)halfedges_around_face.size());

        //
        for (std::vector<halfedge_descriptor_t>::const_iterator h = halfedges_around_face.cbegin();
             h != halfedges_around_face.cend();
             ++h) {
        }
    }

    write_off(name.c_str(), mesh);
}

#if 0
bool point_on_face_plane(const hmesh_t& m, const fd_t& f, const vec3& p, int& fv_count)
{
    const std::vector<vd_t> vertices = m.get_vertices_around_face(f);
    fv_count = (int)vertices.size();
    {
        for (int i = 0; i < fv_count; ++i) {
            const int j = (i + 1) % fv_count;
            const int k = (i + 2) % fv_count;

            const vd_t& vi = vertices[i];
            const vd_t& vj = vertices[j];
            const vd_t& vk = vertices[k];

            const vec3& vi_coords = m.vertex(vi);
            const vec3& vj_coords = m.vertex(vj);
            const vec3& vk_coords = m.vertex(vk);

            const bool are_coplaner = coplaner(vi_coords, vj_coords, vk_coords, p);

            if (!are_coplaner) {
                return false;
            }
        }
    }
    return true;
}
#endif

/*
    dfs(node u)
        for each node v connected to u :
            if v is not visited :
                visited[v] = true
                dfs(v)
*/
void dfs_cc(vd_t u, const hmesh_t& mesh, std::vector<int>& visited, int connected_component_id)
{
    std::vector<vd_t> verts = mesh.get_vertices_around_vertex(u);
    for (std::vector<vd_t>::const_iterator v = verts.cbegin(); v != verts.cend(); ++v) {
        if (SAFE_ACCESS(visited, *v) == -1) {
            visited[*v] = connected_component_id;
            dfs_cc(*v, mesh, visited, connected_component_id);
        }
    }
}

int find_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    thread_pool& scheduler,
#endif
    std::vector<int>& fccmap,
    const hmesh_t& mesh,
    std::vector<int>& cc_to_vertex_count,
    std::vector<int>& cc_to_face_count)
{
    MCUT_ASSERT(mesh.number_of_vertices() >= 3);
    MCUT_ASSERT(mesh.number_of_edges() >= 3);
    MCUT_ASSERT(mesh.number_of_faces() >= 1);

    /*
        for each node u:
            if u is not visited :
                visited[u] = true
                connected_component += 1
                dfs(u)
    */
    std::vector<int> visited(mesh.number_of_vertices(), -1); // if vertex does not exist, then its not visited
    int connected_component_id = -1;
    std::vector<bool> queued(mesh.number_of_vertices(), false);
    std::queue<vd_t> queue; // .. to discover all vertices of current connected component

    std::vector<vd_t> vertices_of_v;
    std::vector<vd_t> vertices_of_u;

    for (vertex_array_iterator_t u = mesh.vertices_begin(); u != mesh.vertices_end(); ++u) {
        if (visited[*u] == -1) {
            connected_component_id += 1;
            visited[*u] = connected_component_id;
            queued[*u] = true;

            cc_to_vertex_count.push_back(1); // each discovered cc has at least one vertex

            vertices_of_u.clear();
            mesh.get_vertices_around_vertex(vertices_of_u, *u);

            for (int i = 0; i < (int)vertices_of_u.size(); ++i) {
                vd_t vou = vertices_of_u[i];
                queue.push(vou);
                queued[vou] = true;
            }

            while (!queue.empty()) {
                vd_t v = queue.front(); // current
                queue.pop();

                if (visited[v] == -1) // v not yet associated with a cc
                {
                    visited[v] = connected_component_id;
                    cc_to_vertex_count[connected_component_id] += 1;

                    vertices_of_v.clear();
                    mesh.get_vertices_around_vertex(vertices_of_v, v);

                    for (int i = 0; i < (int)vertices_of_v.size(); ++i) {
                        vd_t vov = vertices_of_v[i];
                        if (visited[vov] == -1 || queued[vov] == false) {
                            queue.push(vertices_of_v[i]);
                            queued[vov] = true;
                        }
                    }
                }
            }

            // dfs_cc(*u, mesh, visited, connected_component_id);
        }
    }

    fccmap.clear();
    fccmap.resize(mesh.number_of_faces());
    int num_connected_components = (connected_component_id + 1); // number of CCs
    cc_to_face_count.resize(num_connected_components);
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    auto fn_set_cc_to_face_count = [](std::vector<int>::iterator block_start_, std::vector<int>::iterator block_end_) {
        for (std::vector<int>::iterator it = block_start_; it != block_end_; ++it) {
            *it = 0;
        }
    };

    parallel_for(
        scheduler,
        cc_to_face_count.begin(),
        cc_to_face_count.end(),
        fn_set_cc_to_face_count);
#else
    for (int i = 0; i < (int)cc_to_face_count.size(); ++i) {
        cc_to_face_count[i] = 0;
    }
#endif

    fccmap.reserve(mesh.number_of_faces());

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    auto fn_map_faces = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
        for (face_array_iterator_t f = block_start_; f != block_end_; ++f) {
            const std::vector<vertex_descriptor_t> vertices = mesh.get_vertices_around_face(*f);

            int face_cc_id = SAFE_ACCESS(visited, vertices.front());

            // all vertices belong to the same conn comp
            fccmap[*f] = face_cc_id;

            cc_to_face_count[face_cc_id] += 1;
        }
    };

    parallel_for(
        scheduler,
        mesh.faces_begin(),
        mesh.faces_end(),
        fn_map_faces);
#else
    // map each face to a connected component
    for (face_array_iterator_t f = mesh.faces_begin(); f != mesh.faces_end(); ++f) {
        const std::vector<vertex_descriptor_t> vertices = mesh.get_vertices_around_face(*f);

        int face_cc_id = SAFE_ACCESS(visited, vertices.front());

        // all vertices belong to the same conn comp
        fccmap[*f] = face_cc_id;

        cc_to_face_count[face_cc_id] += 1;
    }
#endif

    return num_connected_components;
}

struct connected_component_info_t {
    sm_frag_location_t location = sm_frag_location_t::UNDEFINED; // above/ below
    // vertices along the cut path seam
    std::vector<vd_t> seam_vertices;
    // mapping from mesh descriptors to input mesh descriptors (vertex and face)
    output_mesh_data_maps_t data_maps;
};

// a seam vertex is simply an intersection point, including a duplicated instance if it exists as determined by the
// parameters "ps_num_vertices" and "m1_num_vertices_after_srcmesh_partitioning"
void mark_seam_vertices(
    std::vector<bool>& mesh_seam_vertices,
    hmesh_t& mesh,
    const int ps_num_vertices,
    const int m1_num_vertices_after_srcmesh_partitioning = std::numeric_limits<int>::max())
{
    mesh_seam_vertices.resize(mesh.number_of_vertices());
    for (vertex_array_iterator_t i = mesh.vertices_begin();
         i != mesh.vertices_end();
         ++i) {
        const int idx = static_cast<int>(*i);
        mesh_seam_vertices[*i] = (idx >= ps_num_vertices && idx < m1_num_vertices_after_srcmesh_partitioning);
    }
}

// returns the unseparated/merged connected components
hmesh_t extract_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    thread_pool& scheduler,
#endif
    // key = cc-id; value = list of cc copies each differing by one newly stitched polygon
    std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>& connected_components,
    const hmesh_t& in,
    const int traced_polygons_base_offset,
    const std::vector<std::vector<hd_t>>& mX_traced_polygons, // "m0" or "m1" (dependent on function-call location)
    const std::vector<int>& sm_polygons_below_cs,
    const std::vector<int>& sm_polygons_above_cs,
    const std::vector<bool>& mesh_vertex_to_seam_flag,
    // vertex & face data mapping parameters
    // const std::map<vd_t /*"m1" ovtx in sm*/, vd_t /*"m0" ovtx in sm*/> &m1_to_m0_sm_ovtx_colored,
    const std::vector<vd_t /*"m1" ovtx in sm*/>& m1_to_m0_sm_ovtx_colored,
    const std::unordered_map<vd_t /*"m1" ovtx*/, vd_t /*"m0" ovtx in cm*/>& m1_to_m0_cm_ovtx_colored,
    /*const*/ std::unordered_map<int /*"m0" face idx*/, int /*"m1" face idx*/>& m1_to_m0_face_colored,
    // const std::map<vd_t /*"m0" ovtx*/, vd_t /*"ps" ovtx*/> &m0_to_ps_vtx,
    const std::vector<vd_t>& m0_to_ps_vtx,
    /*const*/ std::unordered_map<int /*"m0" face idx*/, fd_t /*"ps" face*/>& m0_to_ps_face,
    // const std::map<vd_t /*"sm" vtx*/, vd_t /*"ps" vtx*/> &ps_to_sm_vtx,
    const std::vector<vd_t>& ps_to_sm_vtx,
    // const std::map<fd_t /*"sm" face*/, fd_t /*"ps" face*/> &ps_to_sm_face,
    const std::vector<fd_t>& ps_to_sm_face,
    // const std::map<vd_t /*"cm" vtx*/, vd_t /*"ps" vtx*/> &ps_to_cm_vtx,
    const std::vector<vd_t>& ps_to_cm_vtx,
    // const std::map<fd_t /*"cs" face*/, fd_t /*"ps" face*/> &ps_to_cm_face,
    const std::vector<fd_t>& ps_to_cm_face,
    const int sm_vtx_cnt,
    const int sm_face_count,
    bool popuplate_vertex_maps,
    bool popuplate_face_maps,
    bool keep_fragments_below_cutmesh,
    bool keep_fragments_above_cutmesh,
    bool keep_fragments_partially_cut)
{

    // the auxilliary halfedge mesh containing vertices and edges referenced by the traced polygons
    hmesh_t mesh = in; // copy
    mesh.reserve_for_additional_elements((std::uint32_t)mX_traced_polygons.size() / 2);

    ///////////////////////////////////////////////////////////////////////////
    // Insert traced polygons into the auxilliary mesh
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Extract CC: Insert polygons");

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef std::tuple<
            std::vector<std::vector<vd_t>> // "mesh" faces
            >
            OutputStorageTypesTuple;
        typedef std::vector<std::vector<hd_t>>::const_iterator InputStorageIteratorType;

        auto fn_compute_inserted_faces = [&mesh](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageTypesTuple {
            OutputStorageTypesTuple local_output;
            std::vector<std::vector<vd_t>>& faces_LOCAL = std::get<0>(local_output);
            faces_LOCAL.reserve(std::distance(block_start_, block_end_));
            for (InputStorageIteratorType mX_traced_polygons_iter = block_start_; mX_traced_polygons_iter != block_end_; ++mX_traced_polygons_iter) {
                const std::vector<hd_t>& mX_traced_polygon = *mX_traced_polygons_iter;

                faces_LOCAL.push_back(std::vector<vd_t>());
                std::vector<vd_t>& polygon_vertices = faces_LOCAL.back();
                polygon_vertices.reserve(mX_traced_polygon.size());

                // for each halfedge in polygon
                for (std::vector<hd_t>::const_iterator mX_traced_polygon_halfedge_iter = mX_traced_polygon.cbegin();
                     mX_traced_polygon_halfedge_iter != mX_traced_polygon.cend();
                     ++mX_traced_polygon_halfedge_iter) {
                    polygon_vertices.push_back(mesh.target(*mX_traced_polygon_halfedge_iter));
                }
            }

            return local_output;
        };

        std::vector<std::future<OutputStorageTypesTuple>> futures;
        OutputStorageTypesTuple partial_res;

        parallel_for(
            scheduler,
            mX_traced_polygons.cbegin(),
            mX_traced_polygons.cend(),
            fn_compute_inserted_faces,
            partial_res, // output computed by master thread
            futures);

        const std::vector<std::vector<vd_t>>& faces_MASTER_THREAD_LOCAL = std::get<0>(partial_res);

        auto merge_local_faces = [&mesh](const std::vector<std::vector<vd_t>>& faces_) {
            for (std::vector<std::vector<vd_t>>::const_iterator face_iter = faces_.cbegin();
                 face_iter != faces_.cend();
                 ++face_iter) {
                const fd_t f = mesh.add_face(*face_iter);
                MCUT_ASSERT(f != hmesh_t::null_face());
            }
        };

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<OutputStorageTypesTuple>& f = futures[i];
            MCUT_ASSERT(f.valid());
            OutputStorageTypesTuple future_result = f.get(); // "get()" is a blocking function

            const std::vector<std::vector<vd_t>>& faces_FUTURE = std::get<0>(future_result);

            merge_local_faces(faces_FUTURE);
        }

        // merge faces computed by master thread at the end to maintain the same order
        // of the traced polygons
        merge_local_faces(faces_MASTER_THREAD_LOCAL);
    } // endif of parallel scope

#else
    // for each traced polygon
    for (std::vector<std::vector<hd_t>>::const_iterator mX_traced_polygons_iter = mX_traced_polygons.cbegin();
         mX_traced_polygons_iter != mX_traced_polygons.cend();
         ++mX_traced_polygons_iter) {

        // const int polygon_idx = (int)std::distance(traced_polygons.cbegin(), traced_sm_polygon_iter);
        const std::vector<hd_t>& mX_traced_polygon = *mX_traced_polygons_iter;

        //
        // gather polygon's vertices
        //

        std::vector<vd_t> polygon_vertices;
        polygon_vertices.reserve(mX_traced_polygon.size());

        // for each halfedge in polygon
        for (std::vector<hd_t>::const_iterator mX_traced_polygon_halfedge_iter = mX_traced_polygon.cbegin();
             mX_traced_polygon_halfedge_iter != mX_traced_polygon.cend();
             ++mX_traced_polygon_halfedge_iter) {
            polygon_vertices.push_back(mesh.target(*mX_traced_polygon_halfedge_iter));
        }

        // insert face into halfedge data structure
        const fd_t f = mesh.add_face(polygon_vertices);

        // we have violated halfedge data structure construction
        // rules probably because we are refering to a halfedge
        // and its opposite in one polygon
        MCUT_ASSERT(f != hmesh_t::null_face());
    }
#endif
    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // find connected components in "mesh"
    ///////////////////////////////////////////////////////////////////////////

    // connected components
    std::map<std::size_t, std::shared_ptr<hmesh_t>> ccID_to_mesh;
    // location of each connected component w.r.t cut-mesh (above | below | undefined)
    std::map<std::size_t, sm_frag_location_t> ccID_to_cs_descriptor;
    // for each component, we have a map which relates the vertex descriptors (indices) in the
    // auxilliary halfedge data structure "mesh" to the (local) vertex descriptors in
    // the connected-component.
    //
    // the "X" in "...mX_..." stands for "0" or "1" depending on where the current function is called from!
    // Before "m1" is created in "dispatch", X = "0". Afterwards, X == "1" to signify the fact that the
    // input paramater called "in" (in this function) represents "m0" or "m1"
    std::map<std::size_t, std::unordered_map<vd_t, vd_t>> ccID_to_mX_to_cc_vertex;
    // std::map<std::size_t, std::unordered_map<vd_t, vd_t>> ccID_to_cc_to_mX_vertex;
    std::map<std::size_t, std::vector<vd_t>> ccID_to_cc_to_mX_vertex;
    // the vertex descriptors [in the cc] which are seam vertices!
    std::map<std::size_t, std::vector<vd_t>> cc_to_seam_vertices;
    // here we create a map to tag each polygon in "mesh" with the connected component it belongs to.
    std::vector<int> fccmap;

    TIMESTACK_PUSH("Extract CC: find connected components");
    std::vector<int> cc_to_vertex_count;
    std::vector<int> cc_to_face_count;
    find_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        scheduler,
#endif
        fccmap, mesh, cc_to_vertex_count, cc_to_face_count);
    TIMESTACK_POP();

    //

    ///////////////////////////////////////////////////////////////////////////
    // Map vertex descriptors to each connected component
    ///////////////////////////////////////////////////////////////////////////

    // NOTE: even if the number of connected components is one, we proceed anyway
    // because each connected connected excludes unused vertices in "mesh"

    TIMESTACK_PUSH("Extract CC: Map vertices");

    // for each face in the auxilliary mesh (i.e. traced polygon)
    for (face_array_iterator_t face_iter = mesh.faces_begin(); face_iter != mesh.faces_end(); ++face_iter) {

        face_descriptor_t fd = *face_iter;
        const int face_cc_id = SAFE_ACCESS(fccmap, fd); // get connected component of face

        std::map<std::size_t, std::shared_ptr<hmesh_t>>::iterator ccID_to_mesh_fiter = ccID_to_mesh.find(face_cc_id);
        if (ccID_to_mesh_fiter == ccID_to_mesh.end()) {
            // create new mesh to store connected component
            std::pair<std::map<std::size_t, std::shared_ptr<hmesh_t>>::iterator, bool> p = ccID_to_mesh.insert(std::make_pair(face_cc_id, std::shared_ptr<hmesh_t>(new hmesh_t)));
            ccID_to_mesh_fiter = p.first;
        }

        std::shared_ptr<hmesh_t> cc_mesh = ccID_to_mesh_fiter->second;

        std::map<std::size_t, std::unordered_map<vd_t, vd_t>>::iterator ccID_to_mX_to_cc_vertex_fiter = ccID_to_mX_to_cc_vertex.find(face_cc_id);

        if (ccID_to_mX_to_cc_vertex_fiter == ccID_to_mX_to_cc_vertex.end()) {
            // create new component descriptor map
            std::pair<std::map<std::size_t, std::unordered_map<vd_t, vd_t>>::iterator, bool> p = ccID_to_mX_to_cc_vertex.insert(std::make_pair(face_cc_id, std::unordered_map<vd_t, vd_t>()));
            ccID_to_mX_to_cc_vertex_fiter = p.first;
        }

        std::unordered_map<vd_t, vd_t>& mX_to_cc_vertex = ccID_to_mX_to_cc_vertex_fiter->second;

        std::map<std::size_t, std::vector<vd_t>>::iterator ccID_to_cc_to_mX_vertex_fiter = ccID_to_cc_to_mX_vertex.find(face_cc_id);

        if (ccID_to_cc_to_mX_vertex_fiter == ccID_to_cc_to_mX_vertex.end()) {
            std::pair<std::map<std::size_t, std::vector<vd_t>>::iterator, bool> p = ccID_to_cc_to_mX_vertex.insert(std::make_pair(face_cc_id, std::vector<vd_t>()));
            ccID_to_cc_to_mX_vertex_fiter = p.first;
        }

        std::vector<vd_t>& cc_to_mX_vertex = ccID_to_cc_to_mX_vertex_fiter->second;

        std::map<std::size_t, std::vector<vd_t>>::iterator cc_to_seam_vertices_fiter = cc_to_seam_vertices.find(face_cc_id);

        if (cc_to_seam_vertices_fiter == cc_to_seam_vertices.end()) {
            std::pair<std::map<std::size_t, std::vector<vd_t>>::iterator, bool> p = cc_to_seam_vertices.insert(std::make_pair(face_cc_id, std::vector<vd_t>()));
            cc_to_seam_vertices_fiter = p.first;
        }

        std::vector<vd_t>& cc_seam_vertices = cc_to_seam_vertices_fiter->second;

        //
        // Determine the location of the connected component w.r.t the cut-mesh (above/below/undefined)
        //

        // check if the current face is already marked as "below" (w.r.t the cut-mesh).
        const bool cc_is_below_cs = std::binary_search(sm_polygons_below_cs.cbegin(), sm_polygons_below_cs.cend(), static_cast<int>(fd));

        if (cc_is_below_cs) {
            // try to save the fact that the current connected component is "below"
            std::pair<std::map<std::size_t, sm_frag_location_t>::iterator, bool> p = ccID_to_cs_descriptor.insert(std::make_pair(face_cc_id, sm_frag_location_t::BELOW));
            // if 1) insertion did not take place (connected component already registered), and
            // 2) the existing connected component at that entry is marked as "above":
            //  --> partial cut: thus, the notion "above"/"below" is undefined
            if (p.second == false && p.first->second == sm_frag_location_t::ABOVE) {
                // polygon classed as both above and below cs
                // this is because the connected component contains polygons which are both "above"
                // and "below" the cutting surface (we have a partial cut)
                p.first->second = sm_frag_location_t::UNDEFINED;
            }
        }

        // check if connected component is marked as "above"
        const bool cc_is_above_cs = std::binary_search(sm_polygons_above_cs.cbegin(), sm_polygons_above_cs.cend(), static_cast<int>(fd));

        if (cc_is_above_cs) {
            // try to save the fact that the current connected component is tagged "above"
            std::pair<std::map<std::size_t, sm_frag_location_t>::iterator, bool> p = ccID_to_cs_descriptor.insert(std::make_pair(face_cc_id, sm_frag_location_t::ABOVE));
            // if 1) insertion did not take place (connected component is already registered), and
            // 2) the existing connected component at that entry is marked as "below":
            //--> partial cut: connected component has polygon whch are both "above" and "below"
            if (p.second == false && p.first->second == sm_frag_location_t::BELOW) {
                p.first->second = sm_frag_location_t::UNDEFINED; // polygon classed as both above and below cs
            }
        }

        //
        // We now map the vertices of the current face from the auxilliary data
        // structure "mesh" to the (local) connected-component
        //

//#define EXTRACT_SEAM_HALFEDGES

#ifdef EXTRACT_SEAM_HALFEDGES
        std::vector<halfedge_descriptor_t> cc_seam_halfedges;
        const std::vector<halfedge_descriptor_t>& halfedges_on_face = mesh.get_halfedges_around_face(fd);
        bool prev_vertex_belonged_to_seam = false; // previous in face
        #endif
        // for each vertex around the current face
        const std::vector<vertex_descriptor_t> vertices_around_face = mesh.get_vertices_around_face(fd); // order according to "halfedges_on_face" (targets)

        for (std::vector<vertex_descriptor_t>::const_iterator face_vertex_iter = vertices_around_face.cbegin();
             face_vertex_iter != vertices_around_face.cend();
             ++face_vertex_iter) {

            MCUT_ASSERT(ccID_to_mX_to_cc_vertex.find(face_cc_id) != ccID_to_mX_to_cc_vertex.cend());

            // if vertex is not already mapped from "mesh" to connected component
            if (mX_to_cc_vertex.find(*face_vertex_iter) == mX_to_cc_vertex.end()) {

                // MCUT_ASSERT(ccID_to_mesh.find(face_cc_id) != ccID_to_mesh.cend());

                // copy vertex from auxilliary data structure "mesh", add it into connected component mesh,
                // and save the vertex's descriptor in the conected component mesh.
                const vd_t cc_descriptor = cc_mesh->add_vertex(mesh.vertex(*face_vertex_iter));

                // map vertex
                mX_to_cc_vertex.insert(std::make_pair(*face_vertex_iter, cc_descriptor));
                if (popuplate_vertex_maps) {
                    // SAFE_ACCESS(ccID_to_cc_to_mX_vertex, face_cc_id).insert(std::make_pair(cc_descriptor, *face_vertex_iter));
                    cc_to_mX_vertex.push_back(*face_vertex_iter);
                }
                // check if we need to save vertex as being a seam vertex
                // std::vector<bool>::const_iterator fiter = mesh_vertex_to_seam_flag.find(*face_vertex_iter);
                bool is_seam_vertex = (size_t)(*face_vertex_iter) < mesh_vertex_to_seam_flag.size() && SAFE_ACCESS(mesh_vertex_to_seam_flag, *face_vertex_iter); //(size_t)(*face_vertex_iter) < mesh_vertex_to_seam_flag.size(); //fiter != mesh_vertex_to_seam_flag.cend() && fiter->second == true;
                
                if (is_seam_vertex) {
                    
                    cc_seam_vertices.push_back(cc_descriptor);
#ifdef EXTRACT_SEAM_HALFEDGES
                    const uint32_t face_vertex_idx = std::distance(vertices_around_face.cbegin(), face_vertex_iter);

                    const bool is_first_face_vertex = (face_vertex_idx == 0);
                    bool have_seam_halfedge = prev_vertex_belonged_to_seam;

                    if (is_first_face_vertex) {
                        vd_t last_vtx_descr = (*(vertices_around_face.end() - 1));
                        bool last_vertex_is_seam_vertex = (size_t)(last_vtx_descr) < mesh_vertex_to_seam_flag.size() && SAFE_ACCESS(mesh_vertex_to_seam_flag, last_vtx_descr); //(size_t)(*face_vertex_iter) < mesh_vertex_to_seam_flag.size(); //fiter != mesh_vertex_to_seam_flag.cend() && fiter->second == true;
                        have_seam_halfedge = (last_vertex_is_seam_vertex);
                    }
                    
                    if (have_seam_halfedge) {
                        const halfedge_descriptor_t seam_he = SAFE_ACCESS(halfedges_on_face, face_vertex_idx); // number of halfedge == number of vertices in face
                        cc_seam_halfedges.push_back(seam_he);
                    }
                    #endif
                }
#ifdef EXTRACT_SEAM_HALFEDGES
                prev_vertex_belonged_to_seam = is_seam_vertex;
                #endif
            }
        }
    } // for (face_array_iterator_t face_iter = mesh.faces_begin(); face_iter != mesh.faces_end(); ++face_iter)

    TIMESTACK_POP();

    // bool extractingSeams = (m1_num_vertices_after_srcmesh_partitioning == -1);

    // stores a flag per connected component indicating whether we should
    // keep this CC or throw it away, as per user flags.
    std::map<size_t, bool> ccID_to_keepFlag;
    for (std::map<size_t, std::shared_ptr<hmesh_t>>::const_iterator it = ccID_to_mesh.cbegin(); it != ccID_to_mesh.cend(); ++it) {
        int ccID = (int)it->first;
        std::map<std::size_t, sm_frag_location_t>::iterator fiter = ccID_to_cs_descriptor.find(ccID);
        const bool isSeam = (fiter == ccID_to_cs_descriptor.cend()); // Seams have no notion of "location"
        ccID_to_keepFlag[ccID] = isSeam || ((keep_fragments_above_cutmesh && fiter->second == sm_frag_location_t::ABOVE) || //
                                     (keep_fragments_below_cutmesh && fiter->second == sm_frag_location_t::BELOW) || //
                                     (keep_fragments_partially_cut && fiter->second == sm_frag_location_t::UNDEFINED));
    }

    ///////////////////////////////////////////////////////////////////////////
    // TODO: shift the logic to add vertices into CC to here (like for face below)
    ///////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////
    // Insert faces into connected components using mapped vertex descriptors
    ///////////////////////////////////////////////////////////////////////////

    std::map<size_t, std::vector<fd_t>> ccID_to_cc_to_mX_face;
    for (std::map<size_t, std::shared_ptr<hmesh_t>>::const_iterator it = ccID_to_mesh.cbegin(); it != ccID_to_mesh.cend(); ++it) {
        bool userWantsCC = SAFE_ACCESS(ccID_to_keepFlag, it->first);

        if (!userWantsCC) {
            continue;
        }

        ccID_to_cc_to_mX_face[it->first] = std::vector<fd_t>();
    }

    TIMESTACK_PUSH("Extract CC: Map faces");
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef std::tuple<
            std::vector<std::vector<vd_t>>, // remapped_faces, // using remapped cc descriptors
            std::vector<int>, // the cc id of each remapped
            std::vector<fd_t> // the mX mesh i.e. the halfedge data structure we call "mesh" that is remapped
            >
            OutputStorageTypesTuple;
        typedef face_array_iterator_t InputStorageIteratorType;

        auto fn_compute_remapped_cc_faces = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageTypesTuple {
            OutputStorageTypesTuple local_output;
            const uint32_t num_elems = (uint32_t)std::distance(block_start_, block_end_);
            std::vector<std::vector<vd_t>>& remapped_faces_LOCAL = std::get<0>(local_output);
            remapped_faces_LOCAL.reserve(num_elems);
            std::vector<int>& local_remapped_face_to_ccID = std::get<1>(local_output);
            local_remapped_face_to_ccID.reserve(num_elems);
            std::vector<fd_t>& local_remapped_face_to_mX_face = std::get<2>(local_output);
            local_remapped_face_to_mX_face.reserve(num_elems);
            for (face_array_iterator_t face_iter = block_start_; face_iter != block_end_; ++face_iter) {
                face_descriptor_t fd = *face_iter;
                const size_t cc_id = fccmap[fd]; // the connected component which contains the current face

                bool userWantsCC = SAFE_ACCESS(ccID_to_keepFlag, cc_id);

                if (!userWantsCC) {
                    continue;
                }

                remapped_faces_LOCAL.push_back(std::vector<vd_t>());
                std::vector<vd_t>& remapped_face = remapped_faces_LOCAL.back(); // using remapped cc descriptors
                local_remapped_face_to_ccID.push_back((int)cc_id);
                local_remapped_face_to_mX_face.push_back(fd);

                // std::map<size_t, std::vector<fd_t>>::iterator ccID_to_cc_to_mX_face_fiter = ccID_to_cc_to_mX_face.find(cc_id);

                MCUT_ASSERT(ccID_to_cc_to_mX_face.find(cc_id) != ccID_to_cc_to_mX_face.cend());
                // std::vector<fd_t> &cc_to_mX_face = ccID_to_cc_to_mX_face_fiter->second;

                // std::map<std::size_t, std::unordered_map<vd_t, vd_t>>::iterator ccID_to_mX_to_cc_vertex_fiter = ;
                MCUT_ASSERT(ccID_to_mX_to_cc_vertex.find(cc_id) != ccID_to_mX_to_cc_vertex.end());
                std::unordered_map<vd_t, vd_t>& mX_to_cc_vertex = SAFE_ACCESS(ccID_to_mX_to_cc_vertex, cc_id);

                // for each vertex around face
                const std::vector<vertex_descriptor_t> vertices_around_face = mesh.get_vertices_around_face(fd);

                for (std::vector<vertex_descriptor_t>::const_iterator face_vertex_iter = vertices_around_face.cbegin();
                     face_vertex_iter != vertices_around_face.cend();
                     ++face_vertex_iter) {
                    MCUT_ASSERT(ccID_to_mX_to_cc_vertex.find(cc_id) != ccID_to_mX_to_cc_vertex.cend());

                    /*const*/ std::unordered_map<vd_t, vd_t>& vertex_map = mX_to_cc_vertex;
                    const vd_t m1_sm_descr = *face_vertex_iter;

                    MCUT_ASSERT(vertex_map.find(m1_sm_descr) != vertex_map.cend());

                    const vd_t cc_descr = SAFE_ACCESS(vertex_map, m1_sm_descr);
                    remapped_face.push_back(cc_descr);
                }
            }

            return local_output;
        };

        std::vector<std::future<OutputStorageTypesTuple>> futures;
        OutputStorageTypesTuple partial_res;

        parallel_for(
            scheduler,
            mesh.faces_begin(),
            mesh.faces_end(),
            fn_compute_remapped_cc_faces,
            partial_res, // output computed by master thread
            futures);

        // remapped_faces
        const std::vector<std::vector<vd_t>>& remapped_faces_MASTER_THREAD_LOCAL = std::get<0>(partial_res);
        const std::vector<int>& local_remapped_face_to_ccID_MASTER_THREAD_LOCAL = std::get<1>(partial_res);
        const std::vector<fd_t>& local_remapped_face_to_mX_face_MASTER_THREAD_LOCAL = std::get<2>(partial_res);

        auto merge_local_remapped_cc_faces = [](
                                                 const std::vector<std::vector<vd_t>>& remapped_faces_,
                                                 const std::vector<int>& local_remapped_face_to_ccID_,
                                                 const std::vector<fd_t>& local_remapped_face_to_mX_face_,
                                                 const bool popuplate_face_maps,
                                                 std::map<size_t, std::shared_ptr<hmesh_t>>& ccID_to_mesh,
                                                 std::map<size_t, std::vector<fd_t>>& ccID_to_cc_to_mX_face) {
            for (std::vector<std::vector<vd_t>>::const_iterator remapped_face_iter = remapped_faces_.cbegin();
                 remapped_face_iter != remapped_faces_.cend();
                 ++remapped_face_iter) {
                uint32_t idx = (uint32_t)std::distance(remapped_faces_.cbegin(), remapped_face_iter);
                uint32_t remapped_face_cc_id = SAFE_ACCESS(local_remapped_face_to_ccID_, idx);

                MCUT_ASSERT(ccID_to_mesh.find(remapped_face_cc_id) != ccID_to_mesh.end());

                std::shared_ptr<hmesh_t> cc_mesh = SAFE_ACCESS(ccID_to_mesh, remapped_face_cc_id);
                fd_t f = cc_mesh->add_face(*remapped_face_iter); // insert the face

                MCUT_ASSERT(f != hmesh_t::null_face());

                if (popuplate_face_maps) {
                    // NOTE: "mX" refers to our halfedge data structure called "mesh" (see single threaded code)
                    std::vector<fd_t>& cc_to_mX_face = SAFE_ACCESS(ccID_to_cc_to_mX_face, remapped_face_cc_id);
                    MCUT_ASSERT((size_t)f == cc_to_mX_face.size() /*cc_to_mX_face.count(f) == 0*/);
                    const fd_t fd = local_remapped_face_to_mX_face_[idx];
                    // cc_to_mX_face[f] = fd;
                    cc_to_mX_face.push_back(fd);
                }
            }
        };

        merge_local_remapped_cc_faces(
            remapped_faces_MASTER_THREAD_LOCAL,
            local_remapped_face_to_ccID_MASTER_THREAD_LOCAL,
            local_remapped_face_to_mX_face_MASTER_THREAD_LOCAL,
            popuplate_face_maps,
            ccID_to_mesh,
            ccID_to_cc_to_mX_face);

        // merge thread-local output into global data structures
        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<OutputStorageTypesTuple>& f = futures[i];
            MCUT_ASSERT(f.valid());
            OutputStorageTypesTuple future_result = f.get(); // "get()" is a blocking function

            const std::vector<std::vector<vd_t>>& remapped_faces_FUTURE = std::get<0>(future_result);
            const std::vector<int>& local_remapped_face_to_ccID_FUTURE = std::get<1>(future_result);
            const std::vector<fd_t>& local_remapped_face_to_mX_face_FUTURE = std::get<2>(future_result);

            merge_local_remapped_cc_faces(
                remapped_faces_FUTURE,
                local_remapped_face_to_ccID_FUTURE,
                local_remapped_face_to_mX_face_FUTURE,
                popuplate_face_maps,
                ccID_to_mesh,
                ccID_to_cc_to_mX_face);
        }

    } // end of parallel scope
#else
    // for each face in the auxilliary data structure "mesh" (traced polygon)
    for (face_array_iterator_t face_iter = mesh.faces_begin(); face_iter != mesh.faces_end(); ++face_iter) {
        face_descriptor_t fd = *face_iter;
        const size_t cc_id = SAFE_ACCESS(fccmap, fd); // the connected component which contains the current face

        bool userWantsCC = SAFE_ACCESS(ccID_to_keepFlag, cc_id);

        if (!userWantsCC) {
            continue;
        }

        std::vector<vd_t> remapped_face; // using remapped cc descriptors
        std::map<size_t, std::vector<fd_t>>::iterator ccID_to_cc_to_mX_face_fiter = ccID_to_cc_to_mX_face.find(cc_id);
        MCUT_ASSERT(ccID_to_cc_to_mX_face_fiter != ccID_to_cc_to_mX_face.cend());
        std::vector<fd_t>& cc_to_mX_face = ccID_to_cc_to_mX_face_fiter->second;

        // std::map<std::size_t, std::unordered_map<vd_t, vd_t>>::iterator ccID_to_mX_to_cc_vertex_fiter = ;
        MCUT_ASSERT(ccID_to_mX_to_cc_vertex.find(cc_id) != ccID_to_mX_to_cc_vertex.end());
        std::unordered_map<vd_t, vd_t>& mX_to_cc_vertex = SAFE_ACCESS(ccID_to_mX_to_cc_vertex, cc_id);

        // for each vertex around face
        /*const*/ std::vector<vertex_descriptor_t> vertices_around_face = mesh.get_vertices_around_face(fd);

        for (std::vector<vertex_descriptor_t>::/*const_*/ iterator face_vertex_iter = vertices_around_face.begin();
             face_vertex_iter != vertices_around_face.end();
             ++face_vertex_iter) {
            MCUT_ASSERT(ccID_to_mX_to_cc_vertex.find(cc_id) != ccID_to_mX_to_cc_vertex.cend());

            /*const*/ std::unordered_map<vd_t, vd_t>& vertex_map = mX_to_cc_vertex;
            const vd_t m1_sm_descr = *face_vertex_iter;

            MCUT_ASSERT(vertex_map.find(m1_sm_descr) != vertex_map.cend());

            const vd_t cc_descr = SAFE_ACCESS(vertex_map, m1_sm_descr);
            remapped_face.push_back(cc_descr);
        }

        MCUT_ASSERT(ccID_to_mesh.find(cc_id) != ccID_to_mesh.end());

        std::shared_ptr<hmesh_t> cc_mesh = SAFE_ACCESS(ccID_to_mesh, cc_id);
        fd_t f = cc_mesh->add_face(remapped_face); // insert the face

        MCUT_ASSERT(f != hmesh_t::null_face());

        if (popuplate_face_maps) {
            MCUT_ASSERT((size_t)f == cc_to_mX_face.size() /*cc_to_mX_face.count(f) == 0*/);
            // cc_to_mX_face[f] = fd;
            cc_to_mX_face.push_back(fd);
        }
    }
#endif

    TIMESTACK_POP();

    // Note: at this stage we have our connected components (meshes) with their
    // vertices and faces defined

    ///////////////////////////////////////////////////////////////////////////
    // Save the output connected components marked with location
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Extract CC: save CCs with location properties");

    // for each connected component
    for (std::map<std::size_t, std::shared_ptr<hmesh_t>>::const_iterator cc_iter = ccID_to_mesh.cbegin();
         cc_iter != ccID_to_mesh.cend();
         ++cc_iter) {

        const std::size_t& cc_id = cc_iter->first;
        bool userWantsCC = SAFE_ACCESS(ccID_to_keepFlag, cc_id);

        if (!userWantsCC) {
            continue;
        }

        const std::shared_ptr<hmesh_t> cc = cc_iter->second;

        // The boolean is needed to prevent saving duplicate connected components into the vector "connected_components[cc_id]".
        // This can happen because the current function is called for each new cut-mesh polygon that is stitched, during the
        // polygon stitching phases. In the other times when the current function is called, we are guarranteed that
        // "connected_components[cc_id]" is empty.
        //
        // The above has the implication that the newly stitched polygon (during the stitching phase) is added to just [one] of the
        // discovered connected components (which are of a particular color tag), thus leaving the other connected components to be
        // discovered as having exactly the same number of polygons as before since no new polygon has been added to them.
        // So to prevent this connected component dupliction issue, a connected component is only added into "connected_components[cc_id]"
        // if the following hold:
        // 1) "connected_components[cc_id]" is empty (making the added connected component new and unique)
        // 2) the most-recent connected component instance at "connected_components[cc_id].back()" has less faces (in which case, always differing by one)
        //    than the new connected component we wish to add i.e. "cc"
        auto cc_fiter = connected_components.find(cc_id);
        bool proceed_to_save_mesh = cc_fiter == connected_components.cend() || cc_fiter->second.back().first->number_of_faces() != cc->number_of_faces();

        if (proceed_to_save_mesh) {

            sm_frag_location_t location = sm_frag_location_t::UNDEFINED;

            if (!sm_polygons_below_cs.empty() && !sm_polygons_above_cs.empty()) {
                MCUT_ASSERT(ccID_to_cs_descriptor.find(cc_id) != ccID_to_cs_descriptor.cend());
                location = SAFE_ACCESS(ccID_to_cs_descriptor, cc_id);
            }

            connected_component_info_t ccinfo;
            ccinfo.location = location;
            ccinfo.seam_vertices = std::move(SAFE_ACCESS(cc_to_seam_vertices, cc_id));

            //
            // Map vertex and face descriptors to original values in the input source- and cut-mesh
            // For vertices it is only non-intersection points that have defined mapping otherwise
            // the mapped-to value is undefined (hmesh_t::null_vertex())
            //

            const std::vector<vd_t>& cc_to_mX_vertex = SAFE_ACCESS(ccID_to_cc_to_mX_vertex, cc_id);

            if (popuplate_vertex_maps) {
                // map cc vertices to original input mesh
                // -----------------------------------
                ccinfo.data_maps.vertex_map.resize(cc->number_of_vertices());
                for (vertex_array_iterator_t i = cc->vertices_begin(); i != cc->vertices_end(); ++i) {
                    const vd_t cc_descr = *i;
                    MCUT_ASSERT((size_t)cc_descr < cc_to_mX_vertex.size() /*cc_to_mX_vertex.count(cc_descr) == 1*/);
                    const vd_t mX_descr = SAFE_ACCESS(cc_to_mX_vertex, cc_descr);

                    // NOTE: "m1_to_m0_sm_ovtx_colored" contains only non-intersection points from the source mesh
                    // std::vector<vd_t>::const_iterator m1_to_m0_sm_ovtx_colored_fiter = m1_to_m0_sm_ovtx_colored.find(mX_descr);

                    bool is_m1_sm_overtex = (size_t)mX_descr < m1_to_m0_sm_ovtx_colored.size(); // m1_to_m0_sm_ovtx_colored_fiter != m1_to_m0_sm_ovtx_colored.cend();
                    vd_t m0_descr = hmesh_t::null_vertex(); // NOTE: two cut-mesh "m1" original vertices may map to one "m0" vertex (due to winding order duplication)

                    if (is_m1_sm_overtex) {
                        m0_descr = SAFE_ACCESS(m1_to_m0_sm_ovtx_colored, mX_descr); // m1_to_m0_sm_ovtx_colored_fiter->second;
                    } else if (!m1_to_m0_cm_ovtx_colored.empty()) { // are we in the stitching stage..? (calling with "m1")
                        // Lets search through the map "m1_to_m0_cm_ovtx_colored"

                        // NOTE: "m1_to_m0_cm_ovtx_colored" contains only non-intersection points from the cut mesh
                        std::unordered_map<vd_t, vd_t>::const_iterator m1_to_m0_cm_ovtx_colored_fiter = m1_to_m0_cm_ovtx_colored.find(mX_descr);

                        bool is_m1_cm_overtex = m1_to_m0_cm_ovtx_colored_fiter != m1_to_m0_cm_ovtx_colored.cend();

                        if (is_m1_cm_overtex) {
                            m0_descr = m1_to_m0_cm_ovtx_colored_fiter->second;
                        }
                    }

                    if (m0_descr == hmesh_t::null_vertex()) { // if still not found, then we are strictly "mX" polygons is "m0" polygons
                        m0_descr = mX_descr;
                    }

                    const bool vertex_is_in_input_mesh_or_is_intersection_point = (m0_descr != hmesh_t::null_vertex()); // i.e. is it an original vertex (its not an intersection point/along cut-path)

                    if (vertex_is_in_input_mesh_or_is_intersection_point) {
                        // std::map<vd_t, vd_t>::const_iterator m0_to_ps_vtx_fiter = m0_to_ps_vtx.find(m0_descr);
                        bool vertex_is_in_input_mesh = (int)m0_descr < (int)m0_to_ps_vtx.size(); // m0_to_ps_vtx_fiter != m0_to_ps_vtx.cend();
                        vd_t input_mesh_descr = hmesh_t::null_vertex(); // i.e. source-mesh or cut-mesh

                        if (vertex_is_in_input_mesh) {
                            // MCUT_ASSERT(m0_to_ps_vtx.count(m0_descr) == 1);
                            const vd_t ps_descr = SAFE_ACCESS(m0_to_ps_vtx, m0_descr); // m0_to_ps_vtx_fiter->second; // SAFE_ACCESS(m0_to_ps_vtx, m0_descr);
                            // we don't know whether it belongs to cut-mesh patch or source-mesh, so check
                            const bool is_cutmesh_vtx = ps_is_cutmesh_vertex(ps_descr, sm_vtx_cnt);
                            if (is_cutmesh_vtx) {
                                input_mesh_descr = SAFE_ACCESS(ps_to_cm_vtx, ps_descr);
                                // add an offset which allows users to deduce which birth/origin mesh (source or cut mesh) a vertex (map value) belongs to.
                                input_mesh_descr = static_cast<vd_t>(input_mesh_descr + sm_vtx_cnt);
                            } else { // source-mesh vertex
                                input_mesh_descr = SAFE_ACCESS(ps_to_sm_vtx, ps_descr);
                            }
                        }

                        MCUT_ASSERT(SAFE_ACCESS(ccinfo.data_maps.vertex_map, cc_descr) == hmesh_t::null_vertex() /*ccinfo.data_maps.vertex_map.count(cc_descr) == 0*/);
                        ccinfo.data_maps.vertex_map[cc_descr] = input_mesh_descr;
                    }
                }
            } // if (popuplate_vertex_maps) {

            if (popuplate_face_maps) {
                // map face to original input mesh
                // -----------------------------------
                MCUT_ASSERT(ccID_to_cc_to_mX_face.count(cc_id) == 1);

                std::vector<fd_t>& cc_to_mX_face = SAFE_ACCESS(ccID_to_cc_to_mX_face, cc_id);
                ccinfo.data_maps.face_map.resize(cc->number_of_faces());
                for (face_array_iterator_t f = cc->faces_begin(); f != cc->faces_end(); ++f) {
                    const fd_t cc_descr = *f;
                    // account for the fact that the parameter "mX_traced_polygons" may contain only a subset of traced polygons
                    // need this to compute correct polygon index to access std::maps
                    // const fd_t cc_descr_offsetted(traced_polygons_base_offset + static_cast<int>(cc_descr));
                    MCUT_ASSERT((size_t)cc_descr < cc_to_mX_face.size() /*cc_to_mX_face.count(cc_descr) == 1*/);
                    const fd_t mX_descr = SAFE_ACCESS(cc_to_mX_face, cc_descr);
                    const fd_t offsetted_mX_descr(traced_polygons_base_offset + static_cast<int>(mX_descr)); // global traced polygon index
                    int m0_descr = -1;

                    if (m1_to_m0_face_colored.size() > 0) { // are we calling from during the patch stitching phase..?
                        const fd_t m1_descr = offsetted_mX_descr;
                        MCUT_ASSERT(m1_to_m0_face_colored.count(mX_descr) == 1);
                        m0_descr = SAFE_ACCESS(m1_to_m0_face_colored, m1_descr);
                    } else {
                        m0_descr = static_cast<int>(offsetted_mX_descr);
                    }

                    MCUT_ASSERT(m0_to_ps_face.count(m0_descr) == 1);
                    const fd_t ps_descr = SAFE_ACCESS(m0_to_ps_face, m0_descr); // every traced polygon can be mapped back to an input mesh polygon
                    fd_t input_mesh_descr = hmesh_t::null_face();

                    const bool from_cutmesh_face = ps_is_cutmesh_face(ps_descr, sm_face_count);
                    if (from_cutmesh_face) {
                        MCUT_ASSERT((int)ps_descr < (int)ps_to_cm_face.size());
                        input_mesh_descr = SAFE_ACCESS(ps_to_cm_face, ps_descr);
                        // add an offset which allows users to deduce which birth/origin mesh (source or cut mesh) a face (map value) belongs to.
                        input_mesh_descr = static_cast<fd_t>(input_mesh_descr + sm_face_count);
                    } else {
                        MCUT_ASSERT((int)ps_descr < (int)ps_to_sm_face.size());
                        input_mesh_descr = SAFE_ACCESS(ps_to_sm_face, ps_descr);
                    }

                    // map to input mesh face
                    MCUT_ASSERT(SAFE_ACCESS(ccinfo.data_maps.face_map, cc_descr) == hmesh_t::null_face() /* (ccinfo.data_maps.face_map.count(cc_descr) == 0*/);
                    ccinfo.data_maps.face_map[cc_descr] = input_mesh_descr;
                }
            } // if (popuplate_face_maps) {

            connected_components[cc_id].emplace_back(cc, std::move(ccinfo));
        }
    }
    TIMESTACK_POP();

    return (mesh);
}

bool is_virtual_face(const fd_t& face)
{
    return (face == hmesh_t::null_face());
}

/*
    @brief: Given a list of sorted vertices which belong to a histogram bin, check that a
    particular component (x, y, or z) of their coordinates is not the same amongst two or more vertices
    */
bool have_same_coordinate(
    const std::vector<std::pair<vd_t, vec3>>& bin_vertices_sorted,
    const int coordinate_index = 0 // 0 = x, 1 = y, 2 = z component
)
{
    // for each vertex, compare to all others in vector (compare by given component)
    bool is_duplicate = false;
    for (std::vector<std::pair<vd_t, vec3>>::const_iterator i = bin_vertices_sorted.begin(); i != bin_vertices_sorted.end(); ++i) {
        const vec3& vertex_i_coordinates = i->second;
        const double vertex_i_coordinate = vertex_i_coordinates[coordinate_index];
        bool vertex_i_coordinate_is_duplicate = false;

        for (std::vector<std::pair<vd_t, vec3>>::const_iterator j = bin_vertices_sorted.begin(); j != bin_vertices_sorted.end(); ++j) {
            if (j == i) {
                continue; // same vertex, skip
            }

            const vec3& vertex_j_coordinates = j->second;
            const double vertex_j_coordinate = vertex_j_coordinates[coordinate_index];
            vertex_i_coordinate_is_duplicate = (vertex_i_coordinate == vertex_j_coordinate);

            if (vertex_i_coordinate_is_duplicate) {
                is_duplicate = true;
                break;
            }
        }

        if (is_duplicate) {
            break;
        }
    }

    return is_duplicate;
}

// TODO: replace code parts that use "m0_ivtx_to_intersection_registry_entry" with calls to this
// function which is much cheaper
inline bool m0_is_polygon_boundary_halfedge(const hd_t& h, uint32_t m0_num_cutpath_halfedges)
{
    return (uint32_t)h >= m0_num_cutpath_halfedges;
}

inline bool m0_is_polygon_boundary_edge(const ed_t& e, uint32_t m0_num_cutpath_edges)
{
    return (uint32_t)e >= m0_num_cutpath_edges;
}

// point an intersection halfedge to the correct instance of an intersection point
vd_t resolve_intersection_point_descriptor(
    const hmesh_t& ps,
    const hmesh_t& m0,
    hmesh_t& m1,
    const hd_t& m0_h,
    const vd_t& m0_h_tgt,
    const vd_t& m1_h_tgt,
    const bool m0_h_is_ox,
    /*const*/ std::vector<std::vector<int>>& m0_h_to_ply,
    /*const*/ std::unordered_map<vd_t, std::vector<hd_t>>& ivtx_to_incoming_hlist,
    /*const*/ std::unordered_map<hd_t, bool>& m0_sm_ihe_to_flag,
    const std::vector<std::pair<ed_t, fd_t>>& m0_ivtx_to_intersection_registry_entry,
    /*const*/ std::unordered_map<hd_t, hd_t>& m0_to_m1_ihe,
    // const std::map<vd_t, vd_t> &m0_to_ps_vtx,
    const std::vector<vd_t>& m0_to_ps_vtx,
    const int ps_vtx_cnt,
    const int sm_vtx_cnt,
    const int sm_face_count,
    const int m0_num_cutpath_halfedges)
{
    // the descriptor instance we want to return
    vd_t resolved_inst = m1_h_tgt;

    // First, we get list of all other halfedges in (in "m0") whose target-vertex
    // is the same as the target of the current halfedge
    const std::vector<hd_t>& incoming = SAFE_ACCESS(ivtx_to_incoming_hlist, m0_h_tgt);

    // the minimum number of halfedges whose target is "m0_h_tgt"
    // this "minimum" case come from interior edges
    MCUT_ASSERT(incoming.size() >= 2);

    // Second, we will now filter "incoming" (those pointing to "m0_h_tgt") by
    // keeping only the halfedges which are:
    // 1) Processed/transformed (so that we can use it to infer what to do with "resolved_inst")
    // 2) are incident to a traced polygon, and
    // 3) used by a traced polygon of the src-mesh
    //
    // The remaining halfedges will be the ones we can use to infer the correct value of "resolved_inst"

    std::vector<hd_t> halfedges_across_cut_path = incoming;

    // for each halfedge across the cut-path
    for (std::vector<hd_t>::iterator halfedge_across_cut_path_iter = halfedges_across_cut_path.begin();
         halfedge_across_cut_path_iter != halfedges_across_cut_path.end();) {

        const vd_t s = m0.source(*halfedge_across_cut_path_iter);
        const vd_t t = m0.target(*halfedge_across_cut_path_iter);
        const bool s_is_ivtx = m0_is_intersection_point(s, ps_vtx_cnt);
        const bool t_is_ivtx = m0_is_intersection_point(t, ps_vtx_cnt);

        // check if the halfedge is only next to the cut-mesh

        const bool is_ox_cs_h = (!s_is_ivtx && ps_is_cutmesh_vertex(SAFE_ACCESS(m0_to_ps_vtx, s), sm_vtx_cnt));
        const bool is_xo_cs_h = (!t_is_ivtx && ps_is_cutmesh_vertex(SAFE_ACCESS(m0_to_ps_vtx, t), sm_vtx_cnt));
        bool is_strictly_cs_h = is_ox_cs_h || is_xo_cs_h; // check if halfedge is used only by a cut-surface polygon
        const bool is_xx = s_is_ivtx && t_is_ivtx;

        if (!is_strictly_cs_h && is_xx) {
#if 0
                //const hd_t s_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, s);
                //const hd_t t_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, t);
                MCUT_ASSERT((size_t)s - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(s) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                const ed_t s_ps_e = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, s - ps_vtx_cnt).first; //  SAFE_ACCESS(m0_ivtx_to_ps_edge, s); //  ps.edge(s_ps_h);
                MCUT_ASSERT((size_t)t - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(t) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                const ed_t t_ps_e = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, t - ps_vtx_cnt).first; //SAFE_ACCESS(m0_ivtx_to_ps_edge, t); // ps.edge(t_ps_h);
#else
            const bool is_boundary_halfedge = m0_is_polygon_boundary_halfedge(
                *halfedge_across_cut_path_iter,
                m0_num_cutpath_halfedges);
#endif
            const bool oh_is_exterior = is_boundary_halfedge; //(s_ps_e == t_ps_e);                                                   // lays on exterior of ps polygon

            if (oh_is_exterior) {
                MCUT_ASSERT((size_t)s - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(s) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                const ed_t s_ps_e = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)s - ps_vtx_cnt).first; //  SAFE_ACCESS(m0_ivtx_to_ps_edge, s); //  ps.edge(s_ps_h);

                const hd_t s_ps_h0 = ps.halfedge(s_ps_e, 0); // could alternatively use t_ps_e since both he's are part of same edge
                fd_t incident_face = ps.face(s_ps_h0);

                if (incident_face == hmesh_t::null_face()) {
                    const hd_t s_ps_h1 = ps.halfedge(s_ps_e, 1);
                    incident_face = ps.face(s_ps_h1);
                    MCUT_ASSERT(incident_face != hmesh_t::null_face());
                }

                // TODO: use "ps_is_cutmesh_vertex" since it will require using much less queries on ps
                is_strictly_cs_h = ps_is_cutmesh_face(incident_face, sm_face_count);
            }
        }

        // if
        // 1) halfedge strictly belongs to the cut-mesh, OR
        // 2) halfedge is not used for tracing, OR
        // 3) halfedge has not been processed
        if (is_strictly_cs_h || //
            SAFE_ACCESS(m0_h_to_ply, *halfedge_across_cut_path_iter).size() == 0 /*m0_h_to_ply.find(*halfedge_across_cut_path_iter) == m0_h_to_ply.end()*/ || //
            SAFE_ACCESS(m0_sm_ihe_to_flag, *halfedge_across_cut_path_iter) == false) { // is halfedge incident to a traced polygon and is it processed..?
            halfedge_across_cut_path_iter = halfedges_across_cut_path.erase(halfedge_across_cut_path_iter);
        } else {
            ++halfedge_across_cut_path_iter; // next
        }
    }

    // there exists not transformed halfedges connected to the current halfedge
    if (halfedges_across_cut_path.empty()) {
        return resolved_inst; // return the original descriptor
    }

    // At this, point we have found a number of halfedges which share "m0_h_tgt"
    // with the current halfedge. So we need to decide what value (instance) of
    // "m0_h_tgt" we should assign "resolved_inst"

    // We classify "halfedges_across_cut_path" into two sets:
    // 1) "halfedges_on_same_side" (... as m0_h )
    // 2) "halfedges_across_cut_path" (other-side)
    //

    std::vector<hd_t> halfedges_on_same_side;

    if (m0_h_is_ox) {
        //
        // check if the opposite halfedge has been transformed
        //

        // get opposite halfedge of the current halfedge (m0_h)
        const hd_t opp = m0.opposite(m0_h);

        if (SAFE_ACCESS(m0_h_to_ply, opp).size() > 0 /*m0_h_to_ply.find(opp) != m0_h_to_ply.end()*/) { // was the opposite halfedge used to trace a polygon
            // get the previous of the opposite halfedge (because it is one of the "incoming" halfedges)
            const hd_t prv_opp = m0.prev(opp);

            if (SAFE_ACCESS(m0_sm_ihe_to_flag, prv_opp)) { // is halfedge processed
                halfedges_on_same_side.push_back(prv_opp);
                // prv_opp is guarranteed to be in halfedges_on_same_side becz halfedges_across_cut_path is simply a vec of all incoming hes
                halfedges_across_cut_path.erase(std::find(halfedges_across_cut_path.begin(), halfedges_across_cut_path.end(), prv_opp));
            }
        }
    } else if (SAFE_ACCESS(m0_h_to_ply, m0_h).size() == 0 /*m0_h_to_ply.find(m0_h) == m0_h_to_ply.end()*/) // edge-case when src-mesh is not watertight (e.g. test 21)
    {
        MCUT_ASSERT(halfedges_across_cut_path.size() == 1);
        const hd_t& h = halfedges_across_cut_path.front();
        const hd_t& h_proc = SAFE_ACCESS(m0_to_m1_ihe, h);
        vd_t h_tgt = m1.target(h_proc);
        const vd_t tgt_copy = m1.add_vertex(m1.vertex(h_tgt)); // make a copy

        resolved_inst = tgt_copy;
    } else { // then halfedge is either xx or xo (see the conditions with which function is called)

        const hd_t nxt = m0.next(m0_h);
        const hd_t opp_nxt = m0.opposite(nxt);

        // MCUT_ASSERT(opp_nxt != hmesh_t::null_halfedge());

        // if halfedge incident to traced polygon and is it processed
        if (SAFE_ACCESS(m0_h_to_ply, opp_nxt).size() > 0 /*m0_h_to_ply.find(opp_nxt) != m0_h_to_ply.end()*/ && SAFE_ACCESS(m0_sm_ihe_to_flag, opp_nxt)) {

            const vd_t nxt_src = m0_h_tgt; // i.e. m0.source(nxt);
            const vd_t nxt_tgt = m0.target(nxt);
            const bool nxt_src_is_itvx = m0_is_intersection_point(nxt_src, ps_vtx_cnt);
            const bool nxt_tgt_is_itvx = m0_is_intersection_point(nxt_tgt, ps_vtx_cnt);
            const bool nxt_is_xx = nxt_src_is_itvx && nxt_tgt_is_itvx;
            bool on_same_side = true;

            if (nxt_is_xx) {
#if 0
                    //const hd_t nxt_src_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, nxt_src);
                    //const hd_t nxt_tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, nxt_tgt);
                    MCUT_ASSERT((size_t)nxt_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(nxt_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                    const ed_t nxt_src_ps_e = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, nxt_src - ps_vtx_cnt).first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, nxt_src); // ps.edge(nxt_src_ps_h);
                    MCUT_ASSERT((size_t)nxt_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(nxt_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                    const ed_t nxt_tgt_ps_e = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, nxt_tgt - ps_vtx_cnt).first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, nxt_tgt); // ps.edge(nxt_tgt_ps_h);
#else
                const bool is_boundary_halfedge = m0_is_polygon_boundary_halfedge(
                    nxt,
                    m0_num_cutpath_halfedges);
#endif
                const bool nxt_is_exterior = is_boundary_halfedge; //(nxt_src_ps_e == nxt_tgt_ps_e); // lays on exterior of ps polygon

                on_same_side = nxt_is_exterior;
            }

            if (on_same_side) {
                halfedges_on_same_side.push_back(opp_nxt);
                halfedges_across_cut_path.erase(std::find(halfedges_across_cut_path.begin(), halfedges_across_cut_path.end(), opp_nxt));
            }
        }
    }

    //
    // Decide what to do with target(h) i.e. determine the correct value for "resolved_inst"
    //

    if (!halfedges_on_same_side.empty()) { // do we already have a halfedge on the [same side] which is tranformed...?
        const hd_t& ss_h = halfedges_on_same_side.front(); // we can retrieve any one
        const hd_t& ss_h_proc = SAFE_ACCESS(m0_to_m1_ihe, ss_h); // m1 version
        resolved_inst = m1.target(ss_h_proc); // update reference
    } else { // do we already have a halfedge on the [other side] which is tranformed...?

        MCUT_ASSERT(!halfedges_across_cut_path.empty());

        const hd_t& h = halfedges_across_cut_path.front();
        MCUT_ASSERT(m0_to_m1_ihe.find(h) != m0_to_m1_ihe.cend());
        const hd_t& h_proc = SAFE_ACCESS(m0_to_m1_ihe, h);
        MCUT_ASSERT((uint32_t)h_proc < (uint32_t)m1.number_of_halfedges());
        vd_t h_tgt = m1.target(h_proc);
        MCUT_ASSERT((uint32_t)h_tgt < (uint32_t)m1.number_of_vertices());
        const vec3 vertex = m1.vertex(h_tgt);
        const vd_t tgt_copy = m1.add_vertex(vertex); // make a copy

        resolved_inst = tgt_copy;
    }

    return resolved_inst;
};

inline std::vector<fd_t> ps_get_ivtx_registry_entry_faces(const hmesh_t& ps, const std::pair<ed_t, fd_t>& ivtx_registry_entry)
{
    const hd_t h0 = ps.halfedge(ivtx_registry_entry.first, 0);
    const hd_t h1 = ps.halfedge(ivtx_registry_entry.first, 1);
    const fd_t h0_face = ps.face(h0);
    const fd_t h1_face = ps.face(h1);

    return { ivtx_registry_entry.second, h0_face, h1_face };
}

//
// update the m0 edges incident on two given intersecting faces of the polygon soup mesh
// "incident" just means that the edge will be used to clip the face in question.
void update_neighouring_ps_iface_m0_edge_list(
    const vd_t& src_vertex,
    const vd_t& tgt_vertex,
    const hmesh_t& ps,
    const fd_t sm_face,
    const fd_t cs_face,
    const std::vector<std::pair<ed_t, fd_t>>& m0_ivtx_to_intersection_registry_entry,
    std::unordered_map<fd_t, std::vector<ed_t>>& ps_iface_to_m0_edge_list,
    const std::vector<ed_t>& m0_cutpath_edges)
{
    const int ps_vtx_cnt = ps.number_of_vertices();
    // for all neighbours of "sm_face" and "cs_face"
    //  if the face is in the registry of src and tgt vertex
    //      get edge list of face
    //      if list does not already contain new edge
    //          add new edge to list

    std::vector<fd_t> neighbouring_ifaces;
    for (auto neigh_face : { sm_face, cs_face }) {
        const std::vector<face_descriptor_t> faces_around_face = ps.get_faces_around_face(neigh_face);
        neighbouring_ifaces.insert(neighbouring_ifaces.end(), faces_around_face.cbegin(), faces_around_face.cend());
    }

    MCUT_ASSERT((size_t)src_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(src_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
    const std::pair<ed_t, fd_t>& src_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)src_vertex - ps_vtx_cnt);
    const std::vector<fd_t> src_registry = ps_get_ivtx_registry_entry_faces(ps, src_vertex_ipair); // SAFE_ACCESS(m0_ivtx_to_ps_faces, src_vertex);

    MCUT_ASSERT((size_t)tgt_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(tgt_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
    const std::pair<ed_t, fd_t>& tgt_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)tgt_vertex - ps_vtx_cnt);
    const std::vector<fd_t> tgt_registry = ps_get_ivtx_registry_entry_faces(ps, tgt_vertex_ipair); // SAFE_ACCESS(m0_ivtx_to_ps_faces, tgt_vertex);

    // for each face that is a neighbour to either sm-face or cm-face
    for (std::vector<fd_t>::const_iterator neigh_face_it = neighbouring_ifaces.cbegin();
         neigh_face_it != neighbouring_ifaces.cend();
         ++neigh_face_it) {
        const fd_t iface = *neigh_face_it;

        MCUT_ASSERT(iface != sm_face && iface != cs_face);

        const bool in_src_reg = std::find(src_registry.cbegin(), src_registry.cend(), iface) != src_registry.cend();
        const bool in_tgt_reg = std::find(tgt_registry.cbegin(), tgt_registry.cend(), iface) != tgt_registry.cend();

        if (in_src_reg && in_tgt_reg) {
            std::unordered_map<fd_t, std::vector<ed_t>>::iterator fiter = ps_iface_to_m0_edge_list.find(iface);
            bool iface_associated_with_some_edges = true;
            if (fiter == ps_iface_to_m0_edge_list.cend()) {
                // insert
                std::pair<std::unordered_map<fd_t, std::vector<ed_t>>::iterator, bool> p = ps_iface_to_m0_edge_list.insert(std::make_pair(iface, std::vector<ed_t>()));
                MCUT_ASSERT(p.second == true);
                fiter = p.first;
                iface_associated_with_some_edges = false;
            }

            MCUT_ASSERT(fiter != ps_iface_to_m0_edge_list.cend());

            std::vector<ed_t>& iface_m0_edge_list = fiter->second;

            bool associate_iface_with_edge = true;
            if (iface_associated_with_some_edges) {
                bool edge_already_associated_with_iface = std::find(iface_m0_edge_list.cbegin(), iface_m0_edge_list.cend(), m0_cutpath_edges.back()) != iface_m0_edge_list.cend();
                associate_iface_with_edge = !(edge_already_associated_with_iface);
            }

            if (associate_iface_with_edge) {
                iface_m0_edge_list.push_back(m0_cutpath_edges.back());
            }
        }
    }
}

typedef std::vector<hd_t> traced_polygon_t;

bool mesh_is_closed(
#if 0 //defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    thread_pool& scheduler,
#endif
    const hmesh_t& mesh)
{
    bool all_halfedges_incident_to_face = true;
#if 0 // defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        printf("mesh=%d\n", (int)mesh.number_of_halfedges());
        all_halfedges_incident_to_face = parallel_find_if(
                                             scheduler,
                                             mesh.halfedges_begin(),
                                             mesh.halfedges_end(),
                                             [&](hd_t h) {
                                                 const fd_t f = mesh.face(h);
                                                 return (f == hmesh_t::null_face());
                                             })
            == mesh.halfedges_end();
    }
#else
    for (halfedge_array_iterator_t iter = mesh.halfedges_begin(); iter != mesh.halfedges_end(); ++iter) {
        const fd_t f = mesh.face(*iter);
        if (f == hmesh_t::null_face()) {
            all_halfedges_incident_to_face = false;
            break;
        }
    }
#endif
    return all_halfedges_incident_to_face;
}

// TODO: thsi can be improved by comparing based on the largest component of the difference vector
// sort points along a straight line
std::vector<vd_t> linear_projection_sort(const std::vector<std::pair<vd_t, vec3>>& points)
{
    /*
1. pick one point as the origin
2. pick any other point as the vector destination point
3. compute normalize vector from <1> to <2>
4. for each point in list of all points
    a) compute unnormalized vector from <1> to <4>
    b) project a) onto 3) using scalar product, and save result in list
5. sort points according to scalar products values from <4b>
*/
    MCUT_ASSERT(points.size() >= 2);
    const std::vector<std::pair<vd_t, vec3>>::const_iterator origin = points.cbegin();
    const std::vector<std::pair<vd_t, vec3>>::const_iterator dst = points.cbegin() + 1;

    vec3 orig_to_dst_vec = normalize(origin->second - dst->second);

    std::vector<std::pair<vd_t, double>> point_projections;

    for (std::vector<std::pair<vd_t, vec3>>::const_iterator i = points.cbegin(); i != points.cend(); ++i) {
        vec3 orig_to_point_vec = (origin->second - i->second);
        point_projections.emplace_back(i->first, dot_product(orig_to_point_vec, orig_to_dst_vec));
    }

    std::sort(point_projections.begin(), point_projections.end(),
        [&](const std::pair<vd_t, double>& a, const std::pair<vd_t, double>& b) {
            return a.second < b.second;
        });

    std::vector<vd_t> sorted_descriptors;
    for (std::vector<std::pair<vd_t, double>>::const_iterator i = point_projections.cbegin(); i != point_projections.cend(); ++i) {
        sorted_descriptors.push_back(i->first);
    }

    return sorted_descriptors;
}

//
// entry point
//
void dispatch(output_t& output, const input_t& input)
{
    lmsg();

    TIMESTACK_PUSH(__FUNCTION__);

    logger_t& lg = output.logger;
    logger_ptr = &output.logger;
    lg.reset();
    lg.set_verbose(input.verbose);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    output.status.store(status_t::SUCCESS);
#endif

    const hmesh_t& sm = (*input.src_mesh);
    const hmesh_t& cs = (*input.cut_mesh);

    if (input.verbose) {
        dump_mesh(sm, "src-mesh");
        dump_mesh(cs, "cut-mesh");
    }

    const int sm_vtx_cnt = sm.number_of_vertices();
    const int sm_face_count = sm.number_of_faces();
    const int cs_face_count = cs.number_of_faces();
    const int cs_vtx_count = cs.number_of_vertices();

    TIMESTACK_PUSH("Check source mesh is closed");
    const bool sm_is_watertight = mesh_is_closed(
#if 0 //defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        *input.scheduler,
#endif
        sm);

    TIMESTACK_POP();

    TIMESTACK_PUSH("Check cut mesh is closed");
    const bool cm_is_watertight = mesh_is_closed(
#if 0// defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        *input.scheduler,
#endif
        cs);

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // create polygon soup
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Create ps");
    hmesh_t ps = sm; // copy

    ps.reserve_for_additional_elements(cs.number_of_vertices()); // hint

    // std::map<vd_t, vd_t> ps_to_sm_vtx;
    std::vector<vd_t> ps_to_sm_vtx((std::size_t)sm_vtx_cnt + cs.number_of_vertices());
#if 0
    std::iota(std::begin(ps_to_sm_vtx), std::end(ps_to_sm_vtx), vd_t(0));
#else
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        auto fn_iota = [&](vertex_array_iterator_t block_start_, vertex_array_iterator_t block_end_) {
            for (vertex_array_iterator_t v = block_start_; v != block_end_; ++v) {
                ps_to_sm_vtx[*v] = *v;
            }
        };

        parallel_for(
            *input.scheduler,
            sm.vertices_begin(),
            sm.vertices_end(),
            fn_iota);
    }
#else
    for (vertex_array_iterator_t v = sm.vertices_begin(); v != sm.vertices_end(); ++v) {
        ps_to_sm_vtx[*v] = *v; // one to one mapping since ps is initially a copy of sm!
    }
#endif
#endif
    // std::map<fd_t, fd_t> ps_to_sm_face;
    std::vector<fd_t> ps_to_sm_face((std::size_t)sm.number_of_faces() + cs.number_of_faces());
#if 0
    std::iota(std::begin(ps_to_sm_face), std::end(ps_to_sm_face), fd_t(0));
#else
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        auto fn_iota = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
            for (face_array_iterator_t f = block_start_; f != block_end_; ++f) {
                ps_to_sm_face[*f] = *f;
            }
        };

        parallel_for(
            *input.scheduler,
            sm.faces_begin(),
            sm.faces_end(),
            fn_iota);
    }
#else
    for (face_array_iterator_t f = sm.faces_begin(); f != sm.faces_end(); ++f) {
        ps_to_sm_face[*f] = *f; // one to one mapping since ps is initially a copy of sm!
    }
#endif
#endif
    // std::map<vd_t, vd_t> cs_to_ps_vtx;
    // std::map<vd_t, vd_t> ps_to_cm_vtx;
    // std::vector<vd_t> cs_to_ps_vtx(cs.number_of_vertices());
    std::vector<vd_t> ps_to_cm_vtx((std::size_t)sm_vtx_cnt + cs.number_of_vertices());

    // merge cm vertices
    for (auto i = cs.vertices_begin(); i != cs.vertices_end(); ++i) {
        const vd_t v = ps.add_vertex(cs.vertex(*i));

        MCUT_ASSERT(v != hmesh_t::null_vertex());

        // cs_to_ps_vtx.insert(std::make_pair(*i, v));
        // cs_to_ps_vtx[*i] = v;
        ps_to_cm_vtx[v] = *i;
    }

    // std::map<fd_t, fd_t> ps_to_cm_face;
    std::vector<fd_t> ps_to_cm_face((std::size_t)sm_face_count + cs_face_count);

    // merge cm faces
    {
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        {
            auto fn_remap_ps_faces = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
                std::vector<std::pair<fd_t, std::vector<vd_t>>> result(std::distance(block_start_, block_end_));

                uint32_t counter = 0;
                for (face_array_iterator_t i = block_start_; i != block_end_; ++i) {
                    std::pair<fd_t, std::vector<vd_t>>& p = result[counter++];
                    std::vector<vd_t>& remapped_face_vertices = p.second;
                    cs.get_vertices_around_face(remapped_face_vertices, *i, sm_vtx_cnt);
                    p.first = *i;
                }

                return result;
            };

            std::vector<std::future<std::vector<std::pair<fd_t, std::vector<vd_t>>>>> futures;
            std::vector<std::pair<fd_t, std::vector<vd_t>>> master_thread_res;

            parallel_for(
                *input.scheduler,
                cs.faces_begin(),
                cs.faces_end(),
                fn_remap_ps_faces,
                master_thread_res,
                futures);

            auto add_faces = [&](const std::vector<std::pair<fd_t, std::vector<vd_t>>>& remapped_faces) {
                for (std::vector<std::pair<fd_t, std::vector<vd_t>>>::const_iterator it = remapped_faces.cbegin(); it != remapped_faces.cend(); ++it) {
                    const std::pair<fd_t, std::vector<vd_t>>& p = *it;
                    const fd_t f = ps.add_face(p.second);

                    MCUT_ASSERT(f != hmesh_t::null_face());

                    ps_to_cm_face[f] = p.first;
                }
            };

            for (uint32_t i = 0; i < (uint32_t)futures.size(); ++i) {
                const std::vector<std::pair<fd_t, std::vector<vd_t>>> f_res = futures[i].get();
                add_faces(f_res);
            }

            add_faces(master_thread_res);
        }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

        std::vector<vd_t> remapped_face_vertices_tmp;
        for (face_array_iterator_t i = cs.faces_begin(); i != cs.faces_end(); ++i) {
            // std::vector<vd_t> fv = get_vertices_on_face(cs, *i);

            cs.get_vertices_around_face(remapped_face_vertices_tmp, *i, sm_vtx_cnt);
            const std::vector<vd_t>& remapped_face_vertices = remapped_face_vertices_tmp;

            const fd_t f = ps.add_face(remapped_face_vertices);

            MCUT_ASSERT(f != hmesh_t::null_face());

            ps_to_cm_face[f] = *i;
        }
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    }

    TIMESTACK_POP();

    // cs_to_ps_vtx.clear();

    if (input.verbose) {
        dump_mesh(ps, "polygon-soup");
    }

    const int ps_vtx_cnt = ps.number_of_vertices();
    // const int ps_face_cnt = ps.number_of_faces();

    //

    ///////////////////////////////////////////////////////////////////////////
    // create the first auxilliary halfedge data structure ("m0")
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Create m0");
    // The auxilliary data structure stores:
    // 1) vertices of the polygon-soup, including new intersection points
    // 2) Non-intersecting edges of the polygon-soup
    // 3) New edges created from intersection points
    hmesh_t m0;

    // copy ps vertices into the auxilliary mesh (map is used to maintain original vertex order)
    // std::map<vd_t, vd_t> m0_to_ps_vtx;
    std::vector<vd_t> m0_to_ps_vtx; // NOTE: only ps vertices are stored here
    // std::map<vd_t, vd_t> ps_to_m0_vtx;
    std::vector<vd_t> ps_to_m0_vtx((std::size_t)sm_vtx_cnt + cs.number_of_vertices());
    for (auto i = ps.vertices_begin(); i != ps.vertices_end(); ++i) {
        const vd_t v = m0.add_vertex(ps.vertex(*i));

        MCUT_ASSERT(v != hmesh_t::null_vertex());

        // m0_to_ps_vtx.emplace(v, *i);
        m0_to_ps_vtx.emplace_back(*i);
        // ps_to_m0_vtx.emplace(*i, v);
        ps_to_m0_vtx[*i] = v;
    }

    TIMESTACK_POP();
    MCUT_ASSERT(m0.number_of_vertices() == ps_vtx_cnt); // ... because we have only copied vertices

    ///////////////////////////////////////////////////////////////////////////
    // Calculate polygon intersection points
    ///////////////////////////////////////////////////////////////////////////

    std::unordered_map<ed_t, std::vector<fd_t>> ps_edge_face_intersection_pairs;
    
    TIMESTACK_PUSH("Prepare edge-to-face pairs");

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    { // NOTE: parallel implementation is different from sequential one
        typedef std::unordered_map<ed_t, std::vector<fd_t>> OutputStorageType;
        typedef std::map<fd_t, std::vector<fd_t>>::const_iterator InputStorageIteratorType;

        std::vector<std::future<OutputStorageType>> futures;

        auto fn_compute_ps_edge_to_faces_map = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) {
            std::unordered_map<ed_t, std::vector<fd_t>> ps_edge_face_intersection_pairs_local;

            for (InputStorageIteratorType iter = block_start_; iter != block_end_; ++iter) {
                // the face with the intersecting edges (i.e. the edges to be tested against the other face)
                const fd_t& intersecting_edge_face = iter->first; // sm_face != hmesh_t::null_face() ? sm_face : cm_face;
                const std::vector<hd_t>& halfedges = ps.get_halfedges_around_face(intersecting_edge_face);

                for (std::vector<hd_t>::const_iterator hIter = halfedges.cbegin(); hIter != halfedges.cend(); ++hIter) {
                    const ed_t edge = ps.edge(*hIter);
                    std::vector<fd_t>& edge_ifaces = ps_edge_face_intersection_pairs_local[edge];
                    if (edge_ifaces.empty()) {
                        edge_ifaces = iter->second;
                        if (edge_ifaces.size() > 1) {
                            std::sort(edge_ifaces.begin(), edge_ifaces.end()); // alows us to do binary search (std::lower_bound)
                        }
                    } else {
                        for (std::vector<fd_t>::const_iterator iface_iter = iter->second.cbegin();
                             iface_iter != iter->second.cend();
                             ++iface_iter) {
                            std::vector<fd_t>::iterator fiter = std::lower_bound(edge_ifaces.begin(), edge_ifaces.end(), *iface_iter);
                            bool exists = fiter != edge_ifaces.end() && (*fiter == *iface_iter);
                            if (!exists) {
                                edge_ifaces.insert(fiter, *iface_iter); // insert and maintain sorted order
                            }
                        }
                    }
                }
            }
            return ps_edge_face_intersection_pairs_local;
        };

        parallel_for(
            *input.scheduler,
            input.ps_face_to_potentially_intersecting_others->cbegin(),
            input.ps_face_to_potentially_intersecting_others->cend(),
            fn_compute_ps_edge_to_faces_map,
            ps_edge_face_intersection_pairs, // out
            futures);

        // merge results from other threads

        for (int fi = 0; fi < (int)futures.size(); ++fi) {
            std::future<OutputStorageType>& f = futures[fi];
            MCUT_ASSERT(f.valid()); // The behavior is undefined if valid()== false before the call to wait_for

            OutputStorageType future_res = f.get();
            // merge results for current block
            for (OutputStorageType::const_iterator i = future_res.cbegin(); i != future_res.cend(); ++i) {
                OutputStorageType::iterator fiter = ps_edge_face_intersection_pairs.find(i->first);
                if (fiter == ps_edge_face_intersection_pairs.cend()) {
                    ps_edge_face_intersection_pairs[i->first] = i->second;
                } else {
                    for (std::vector<fd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                        std::vector<fd_t>::iterator lb_iter = std::lower_bound(fiter->second.begin(), fiter->second.end(), *j);
                        bool exists = lb_iter != fiter->second.end() && (*lb_iter == *j);
                        if (!exists) {
                            fiter->second.insert(lb_iter, *j); // insert and maintain sorted order
                        }
                    }
                }
            }
        }
    } // end of parallel code
#else
    {

        std::vector<fd_t> unvisited_ps_ifaces; //= *input.ps_face_to_potentially_intersecting_others;
        unvisited_ps_ifaces.reserve(input.ps_face_to_potentially_intersecting_others->size());
        // NOTE: the elements of "unvisited_ps_ifaces" are already sorted because they come directly from
        // "input.ps_face_to_potentially_intersecting_others", which is an std::map (keys are always sorted)
        std::transform(
            input.ps_face_to_potentially_intersecting_others->cbegin(),
            input.ps_face_to_potentially_intersecting_others->cend(),
            std::back_inserter(unvisited_ps_ifaces),
            [](const std::pair<fd_t, std::vector<fd_t>>& kv) { return kv.first; });

        std::vector<bool> ps_iface_enqueued(ps.number_of_faces(), false);

        std::vector<bool> ps_edge_visited(ps.number_of_edges(), false);
        // initially null
        std::map<fd_t, std::vector<fd_t>>::const_iterator cur_ps_cc_face = input.ps_face_to_potentially_intersecting_others->cend();
        // start with any face, but we choose the first
        std::map<fd_t, std::vector<fd_t>>::const_iterator next_ps_cc_face = input.ps_face_to_potentially_intersecting_others->cbegin();
        ps_iface_enqueued[next_ps_cc_face->first] = true;

        // an element of this queue is an iterator/ptr to an element of "input.ps_face_to_potentially_intersecting_others"
        std::queue<std::map<fd_t, std::vector<fd_t>>::const_iterator> adj_ps_face_queue;

        do { // each iteration will find a set of edges that belong to a connected-component patch of intersectng faces (of sm or cm) in ps
            cur_ps_cc_face = next_ps_cc_face;
            next_ps_cc_face = input.ps_face_to_potentially_intersecting_others->cend(); // set null

            // register unique edges of current face, and the add the neighbouring faces to queue

            adj_ps_face_queue.push(cur_ps_cc_face);

            do { // each interation will add unregistered edges of current face, and add unvisited faces to queue

                const std::map<fd_t, std::vector<fd_t>>::const_iterator cc_iface = adj_ps_face_queue.front(); // current face of connected-component patch
                adj_ps_face_queue.pop();

                { // face is now visisted so we remove it
                    std::vector<fd_t>::iterator fiter = std::lower_bound(
                        unvisited_ps_ifaces.begin(),
                        unvisited_ps_ifaces.end(),
                        cc_iface->first);
                    MCUT_ASSERT(fiter != unvisited_ps_ifaces.cend());
                    unvisited_ps_ifaces.erase(fiter); // NOTE: list remains sorted
                }

                std::vector<fd_t> cur_ps_face_ifaces_sorted = cc_iface->second; // copy
                if (cur_ps_face_ifaces_sorted.size() > 1) {
                    std::sort(cur_ps_face_ifaces_sorted.begin(), cur_ps_face_ifaces_sorted.end()); // allows quick binary search
                }
                // bool is_sm_face = cc_iface->first < sm_face_count;
                //  const fd_t cc_iface_descr = is_sm_face ? cc_iface->first - sm_face_count : sm_face_count;

                const std::vector<hd_t>& cur_ps_face_halfedges = ps.get_halfedges_around_face(cc_iface->first);
                // all neighbours
                const std::vector<fd_t> cur_ps_face_neigh_faces = ps.get_faces_around_face(cc_iface->first, &cur_ps_face_halfedges);
                // neighbours [which are intersecting faces]
                std::vector<fd_t> cur_ps_face_neigh_ifaces;
                cur_ps_face_neigh_ifaces.reserve(cur_ps_face_neigh_faces.size());

                for (std::vector<fd_t>::const_iterator face_iter = cur_ps_face_neigh_faces.cbegin();
                     face_iter != cur_ps_face_neigh_faces.cend();
                     ++face_iter) {
                    bool is_iface = input.ps_face_to_potentially_intersecting_others->find(*face_iter) != input.ps_face_to_potentially_intersecting_others->cend();
                    if (is_iface) {
                        cur_ps_face_neigh_ifaces.push_back(*face_iter);
                    }
                }

                if (cur_ps_face_neigh_ifaces.size() > 1) {
                    std::sort(cur_ps_face_neigh_ifaces.begin(), cur_ps_face_neigh_ifaces.end());
                }

                // for each halfedge of current iface
                for (std::vector<hd_t>::const_iterator hiter = cur_ps_face_halfedges.cbegin(); hiter != cur_ps_face_halfedges.cend(); ++hiter) {
                    const ed_t halfedge_edge = ps.edge(*hiter);
                    const hd_t opp_he = ps.opposite(*hiter);
                    // Here we simply access corresponding element in "cur_ps_face_neigh_faces" based on
                    // how "ps.get_faces_around_face" populates "cur_ps_face_neigh_faces", given "cur_ps_face_halfedges"
                    // as done above

                    if (ps_edge_visited[halfedge_edge] == false) {
                        ps_edge_face_intersection_pairs.insert(std::make_pair(halfedge_edge, cur_ps_face_ifaces_sorted)); // add edge
                        ps_edge_visited[halfedge_edge] = true;

                        bool is_border_ps_face = cur_ps_face_neigh_faces.size() != cur_ps_face_halfedges.size();
                        const size_t idx = std::distance(cur_ps_face_halfedges.cbegin(), hiter);
                        fd_t opp_he_face = is_border_ps_face ? ps.face(opp_he) : SAFE_ACCESS(cur_ps_face_neigh_faces, idx);

                        if (!is_virtual_face(opp_he_face) && ps_iface_enqueued[opp_he_face] == false) { // two neighbouring faces might share more that 1 edge (case of non-triangulated mesh)
                            bool is_iface = std::binary_search(cur_ps_face_neigh_ifaces.cbegin(), cur_ps_face_neigh_ifaces.cend(), opp_he_face);
                            if (is_iface) {
                                std::map<fd_t, std::vector<fd_t>>::const_iterator fiter = input.ps_face_to_potentially_intersecting_others->find(opp_he_face);
                                adj_ps_face_queue.push(fiter);
                                ps_iface_enqueued[opp_he_face] = true;
                            }
                        }
                    } else {
                        MCUT_ASSERT(ps_edge_face_intersection_pairs.find(halfedge_edge) != ps_edge_face_intersection_pairs.cend());
                        // merge shared "intersected" faces.
                        // Two intersecting faces that share an edge will share intersected faces. The shared faces
                        // are those intersected by the shared edge.
                        std::vector<fd_t>& existing_edge_ifaces = ps_edge_face_intersection_pairs[halfedge_edge]; // sorted list

                        for (std::vector<fd_t>::const_iterator i = cur_ps_face_ifaces_sorted.cbegin(); i != cur_ps_face_ifaces_sorted.cend(); ++i) {
                            std::vector<fd_t>::iterator iter = std::lower_bound(existing_edge_ifaces.begin(), existing_edge_ifaces.end(), *i);

                            bool found = iter != existing_edge_ifaces.end() && iter != existing_edge_ifaces.end() && (*iter == *i);
                            if (!found) {
                                existing_edge_ifaces.insert(iter, *i); // insert into sorted list (i.e. possibly shifts some elements forward)
                            }
                        }
                    }
                } // for each halfedge of current iface

            } while (adj_ps_face_queue.empty() == false);

            // find "next_ps_cc_face" as any face in "input.ps_face_to_potentially_intersecting_others" that is not visited
            if (unvisited_ps_ifaces.size() > 0) {
                fd_t next_face = unvisited_ps_ifaces.back(); // pick any unvisited iface (we choose the last for faster elemt removal from std::vector)
                next_ps_cc_face = input.ps_face_to_potentially_intersecting_others->find(next_face);
                MCUT_ASSERT(next_ps_cc_face != input.ps_face_to_potentially_intersecting_others->cend());
            }

        } while (next_ps_cc_face != input.ps_face_to_potentially_intersecting_others->cend());
    }
    // std::unordered_map<ed_t, std::vector<fd_t>> ps_edge_face_intersection_pairs;
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    TIMESTACK_POP();

    //
    // build bounding boxes for each intersecting edge
    //
#if 1
    TIMESTACK_PUSH("Build edge bounding boxes");

    // http://gamma.cs.unc.edu/RTRI/i3d08_RTRI.pdf
    std::unordered_map<ed_t, bounding_box_t<vec3>> ps_edge_to_bbox;

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef std::unordered_map<ed_t, bounding_box_t<vec3>> OutputStorageType;
        typedef std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator InputStorageIteratorType;

        auto fn_compute_ps_edge_bbox = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) {
            OutputStorageType ps_edge_to_bbox_local;

            for (std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator iedge_iter = block_start_; iedge_iter != block_end_; iedge_iter++) {
                const ed_t edge = iedge_iter->first;
                const vd_t v0 = ps.vertex(edge, 0);
                const vd_t v1 = ps.vertex(edge, 1);
                bounding_box_t<vec3>& edge_bbox = ps_edge_to_bbox_local[edge];
                edge_bbox.expand(ps.vertex(v0));
                edge_bbox.expand(ps.vertex(v1));
            }

            return ps_edge_to_bbox_local;
        };

        std::vector<std::future<OutputStorageType>> futures;

        parallel_for(
            *input.scheduler,
            ps_edge_face_intersection_pairs.cbegin(),
            ps_edge_face_intersection_pairs.cend(),
            fn_compute_ps_edge_bbox,
            ps_edge_to_bbox, // out
            futures);

        // merge results from other threads

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<OutputStorageType>& f = futures[i];
            MCUT_ASSERT(f.valid()); // The behavior is undefined if valid()== false before the call to wait_for

            OutputStorageType future_res = f.get();

            ps_edge_to_bbox.insert(future_res.cbegin(), future_res.cend());
        }
    }
#else
    for (std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator iedge_iter = ps_edge_face_intersection_pairs.cbegin();
         iedge_iter != ps_edge_face_intersection_pairs.cend();
         iedge_iter++) {
        MCUT_ASSERT(iedge_iter->second.size() >= 1);
        const ed_t edge = iedge_iter->first;
        const vd_t v0 = ps.vertex(edge, 0);
        const vd_t v1 = ps.vertex(edge, 1);
        bounding_box_t<vec3>& edge_bbox = ps_edge_to_bbox[edge];
        edge_bbox.expand(ps.vertex(v0));
        edge_bbox.expand(ps.vertex(v1));
    }
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

    TIMESTACK_POP();

    //
    // cull redundant edge to face pairs
    //
    TIMESTACK_PUSH("Cull redundant edge-face pairs");

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef std::unordered_map<ed_t, std::vector<fd_t>>::iterator InputStorageIteratorType;

        auto fn_compute_edgefair_pair_culling = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) {
            for (std::unordered_map<ed_t, std::vector<fd_t>>::iterator iedge_iter = block_start_; iedge_iter != block_end_; iedge_iter++) {
                const ed_t edge = iedge_iter->first;
                const bounding_box_t<vec3>& edge_bbox = ps_edge_to_bbox[edge];
                std::vector<fd_t>& edge_ifaces = iedge_iter->second;

                for (std::vector<fd_t>::iterator iface_iter = edge_ifaces.begin(); iface_iter != edge_ifaces.end(); /*increment inside loop*/) {
                    const bounding_box_t<vec3>* iface_bbox = nullptr;
                    bool is_sm_face = (size_t)(*iface_iter) < (size_t)sm_face_count;
                    if (is_sm_face) {
#if defined(USE_OIBVH)
                        iface_bbox = &(SAFE_ACCESS((*input.source_hmesh_face_aabb_array_ptr), *iface_iter));

#else
                        iface_bbox = &input.source_hmesh_BVH->GetPrimitiveBBox(*iface_iter); // SAFE_ACCESS(((*input.source_hmesh_face_aabb_array_ptr), *iface_iter));
#endif
                    } else {
#if defined(USE_OIBVH)
                        iface_bbox = &(SAFE_ACCESS((*input.cut_hmesh_face_aabb_array_ptr), (size_t)(*iface_iter) - sm_face_count));
#else
                        iface_bbox = &input.cut_hmesh_BVH->GetPrimitiveBBox((size_t)(*iface_iter) - sm_face_count); // SAFE_ACCESS(((*input.cut_hmesh_face_aabb_array_ptr), ((size_t)(*iface_iter) - sm_face_count)));

#endif
                    }

                    bool intersect = intersect_bounding_boxes(edge_bbox, *iface_bbox);

                    if (!intersect) {
                        // remove because "iface_iter" was paired with a coincident face (of "edge") based on
                        // the mere fact that the coincident face was found to be in close proximity with
                        // "iface_iter" (from BVH tree proximity search)
                        iface_iter = edge_ifaces.erase(iface_iter); // NOTE: "erase" return iterator to next after
                    } else {
                        iface_iter++;
                    }
                }
            }
            return 0;
        };

        std::vector<std::future<int>> futures;
        int _1;

        parallel_for(
            *input.scheduler,
            ps_edge_face_intersection_pairs.begin(),
            ps_edge_face_intersection_pairs.end(),
            fn_compute_edgefair_pair_culling,
            _1, // out
            futures);

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<int>& f = futures[i];
            MCUT_ASSERT(f.valid());
            f.wait(); // simply wait for result to be done
        }
    }
#else
    for (std::unordered_map<ed_t, std::vector<fd_t>>::iterator iedge_iter = ps_edge_face_intersection_pairs.begin();
         iedge_iter != ps_edge_face_intersection_pairs.end();
         iedge_iter++) {
        const ed_t edge = iedge_iter->first;
        const bounding_box_t<vec3>& edge_bbox = ps_edge_to_bbox[edge];
        std::vector<fd_t>& edge_ifaces = iedge_iter->second;

        for (std::vector<fd_t>::iterator iface_iter = edge_ifaces.begin(); iface_iter != edge_ifaces.end(); /*increment inside loop*/) {
            const bounding_box_t<vec3>* iface_bbox = nullptr;
            bool is_sm_face = (size_t)(*iface_iter) < (size_t)sm_face_count;
            if (is_sm_face) {
#if defined(USE_OIBVH)
                iface_bbox = SAFE_ACCESS(&((*input.source_hmesh_face_aabb_array_ptr)), *iface_iter);

#else
                iface_bbox = &input.source_hmesh_BVH->GetPrimitiveBBox(*iface_iter); // SAFE_ACCESS(((*input.source_hmesh_face_aabb_array_ptr), *iface_iter));
#endif
            } else {
#if defined(USE_OIBVH)
                iface_bbox = SAFE_ACCESS(&((*input.cut_hmesh_face_aabb_array_ptr)), ((size_t)(*iface_iter) - sm_face_count));
#else
                iface_bbox = &input.cut_hmesh_BVH->GetPrimitiveBBox((size_t)(*iface_iter) - sm_face_count); // SAFE_ACCESS(((*input.cut_hmesh_face_aabb_array_ptr), ((size_t)(*iface_iter) - sm_face_count)));

#endif
            }

            bool intersect = intersect_bounding_boxes(edge_bbox, *iface_bbox);

            if (!intersect) {
                // remove because "iface_iter" was paired with a coincident face (of "edge") based on
                // the mere fact that the coincident face was found to be in close proximity with
                // "iface_iter" (from BVH tree proximity search)
                iface_iter = edge_ifaces.erase(iface_iter); // NOTE: "erase" return iterator to next after
            } else {
                iface_iter++;
            }
        }
    }
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    TIMESTACK_POP();

    ps_edge_to_bbox.clear();

    // assuming each edge will produce a new vertex
    m0.reserve_for_additional_elements((std::uint32_t)ps_edge_face_intersection_pairs.size());
#endif
    TIMESTACK_PUSH("Compute intersecting face properties");
    // compute/extract geometry properties of each tested face
    //--------------------------------------------------------

    std::unordered_map<fd_t, vec3> ps_tested_face_to_plane_normal;
    std::unordered_map<fd_t, double> ps_tested_face_to_plane_normal_d_param;
    std::unordered_map<fd_t, int> ps_tested_face_to_plane_normal_max_comp;
    std::unordered_map<fd_t, std::vector<vec3>> ps_tested_face_to_vertices;

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef std::tuple<
            std::unordered_map<fd_t, vec3>, // ps_tested_face_to_plane_normal;
            std::unordered_map<fd_t, double>, // ps_tested_face_to_plane_normal_d_param;
            std::unordered_map<fd_t, int>, // ps_tested_face_to_plane_normal_max_comp;
            std::unordered_map<fd_t, std::vector<vec3>> // ps_tested_face_to_vertices;
            >
            OutputStorageTypesTuple;
        typedef std::map<fd_t, std::vector<fd_t>>::const_iterator InputStorageIteratorType;

        std::atomic<int> potentially_intersecting_face_with_zero_area(-1); // did any errors occur (e.g. found a face with zero area)

        auto fn_compute_intersecting_face_properties = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageTypesTuple {
            OutputStorageTypesTuple output_res;
            std::unordered_map<fd_t, vec3>& ps_tested_face_to_plane_normal_LOCAL = std::get<0>(output_res);
            std::unordered_map<fd_t, double>& ps_tested_face_to_plane_normal_d_param_LOCAL = std::get<1>(output_res);
            std::unordered_map<fd_t, int>& ps_tested_face_to_plane_normal_max_comp_LOCAL = std::get<2>(output_res);
            std::unordered_map<fd_t, std::vector<vec3>>& ps_tested_face_to_vertices_LOCAL = std::get<3>(output_res);
            std::vector<vd_t> tested_face_descriptors_tmp;
            for (std::map<fd_t, std::vector<fd_t>>::const_iterator tested_faces_iter = block_start_;
                 tested_faces_iter != block_end_;
                 tested_faces_iter++) {
                // get the vertices of tested_face (used to estimate its normal etc.)
                ps.get_vertices_around_face(tested_face_descriptors_tmp, tested_faces_iter->first);
                std::vector<vd_t>& tested_face_descriptors = tested_face_descriptors_tmp;
                std::vector<vec3>& tested_face_vertices = ps_tested_face_to_vertices_LOCAL[tested_faces_iter->first]; // insert and get reference

                for (std::vector<vd_t>::const_iterator it = tested_face_descriptors.cbegin(); it != tested_face_descriptors.cend(); ++it) {
                    const vec3& vertex = ps.vertex(*it);
                    tested_face_vertices.push_back(vertex);
                }

                vec3& tested_face_plane_normal = ps_tested_face_to_plane_normal_LOCAL[tested_faces_iter->first];
                double& tested_face_plane_param_d = ps_tested_face_to_plane_normal_d_param_LOCAL[tested_faces_iter->first];
                int& tested_face_plane_normal_max_comp = ps_tested_face_to_plane_normal_max_comp_LOCAL[tested_faces_iter->first];

                tested_face_plane_normal_max_comp = compute_polygon_plane_coefficients(
                    tested_face_plane_normal,
                    tested_face_plane_param_d,
                    tested_face_vertices.data(),
                    (int)tested_face_vertices.size());

                if (squared_length(tested_face_plane_normal) == 0) {
                    potentially_intersecting_face_with_zero_area.store((int)tested_faces_iter->first, std::memory_order_release);
                }
            }
            return output_res;
        };

        std::vector<std::future<OutputStorageTypesTuple>> futures;
        OutputStorageTypesTuple partial_res;

        parallel_for(
            *input.scheduler,
            input.ps_face_to_potentially_intersecting_others->cbegin(),
            input.ps_face_to_potentially_intersecting_others->cend(),
            fn_compute_intersecting_face_properties,
            partial_res, // out
            futures);

        std::tie(
            ps_tested_face_to_plane_normal,
            ps_tested_face_to_plane_normal_d_param,
            ps_tested_face_to_plane_normal_max_comp,
            ps_tested_face_to_vertices)
            = partial_res;
        // merge results from other threads

        for (int i = 0; i < (int)futures.size(); ++i) {

            std::future<OutputStorageTypesTuple>& f = futures[i];
            MCUT_ASSERT(f.valid()); // The behavior is undefined if valid()== false before the call to wait_for

            OutputStorageTypesTuple future_res = f.get();

            if (potentially_intersecting_face_with_zero_area.load(std::memory_order_acquire) >= 0) {
                break; // stop there was a runtime error
            }

            std::unordered_map<fd_t, vec3>& ps_tested_face_to_plane_normal_FUTURE = std::get<0>(future_res);
            std::unordered_map<fd_t, double>& ps_tested_face_to_plane_normal_d_param_FUTURE = std::get<1>(future_res);
            std::unordered_map<fd_t, int>& ps_tested_face_to_plane_normal_max_comp_FUTURE = std::get<2>(future_res);
            std::unordered_map<fd_t, std::vector<vec3>>& ps_tested_face_to_vertices_FUTURE = std::get<3>(future_res);

            ps_tested_face_to_plane_normal.insert(
                ps_tested_face_to_plane_normal_FUTURE.cbegin(),
                ps_tested_face_to_plane_normal_FUTURE.cend());

            ps_tested_face_to_plane_normal_d_param.insert(
                ps_tested_face_to_plane_normal_d_param_FUTURE.cbegin(),
                ps_tested_face_to_plane_normal_d_param_FUTURE.cend());

            ps_tested_face_to_plane_normal_max_comp.insert(
                ps_tested_face_to_plane_normal_max_comp_FUTURE.cbegin(),
                ps_tested_face_to_plane_normal_max_comp_FUTURE.cend());

            ps_tested_face_to_vertices.insert(
                ps_tested_face_to_vertices_FUTURE.cbegin(),
                ps_tested_face_to_vertices_FUTURE.cend());
        }

        const int tmp_local = potentially_intersecting_face_with_zero_area.load(std::memory_order_acquire);

        if (tmp_local >= 0) {
            const bool is_cutmesh_face = (tmp_local > sm_face_count);
            // if "tmp_local" > srcMeshFaceCount then "tmp_local" is a cut-mesh face with id="tmp_local-srcMeshFaceCount"
            const std::string msh_name = is_cutmesh_face ? "cut-mesh" : "source-mesh";
            // index/descriptor in the _kernel_ input mesh (note the stress on kernel since frontend might modify user-provided mesh)
            const fd_t bad_face_desr = fd_t(is_cutmesh_face ? (tmp_local - sm_face_count) : tmp_local);
            lg.set_reason_for_failure("face f" + std::to_string(bad_face_desr) + " of " + msh_name + " is degenerate (has zero area)");
            output.status.store(is_cutmesh_face ? status_t::INVALID_CUT_MESH : status_t::INVALID_SRC_MESH, std::memory_order_release);
            return; // stop there was a runtime error
        }

    } // end of parallel scope
#else
    // for each face that is to be tested for intersection
    // NOTE: the keys of input.ps_face_to_potentially_intersecting_others are the potentially colliding polygons
    // that we get after BVH traversal
    {
        std::vector<vd_t> tested_face_descriptors_tmp;
        for (std::map<fd_t, std::vector<fd_t>>::const_iterator tested_faces_iter = input.ps_face_to_potentially_intersecting_others->cbegin();
             tested_faces_iter != input.ps_face_to_potentially_intersecting_others->cend();
             tested_faces_iter++) {
            // get the vertices of tested_face (used to estimate its normal etc.)
            ps.get_vertices_around_face(tested_face_descriptors_tmp, tested_faces_iter->first);
            const std::vector<vd_t>& tested_face_descriptors = tested_face_descriptors_tmp;
            std::vector<vec3>& tested_face_vertices = ps_tested_face_to_vertices[tested_faces_iter->first]; // insert and get reference

            for (std::vector<vd_t>::const_iterator it = tested_face_descriptors.cbegin(); it != tested_face_descriptors.cend(); ++it) {
                const vec3& vertex = ps.vertex(*it);
                tested_face_vertices.push_back(vertex);
            }

            vec3& tested_face_plane_normal = ps_tested_face_to_plane_normal[tested_faces_iter->first];
            double& tested_face_plane_param_d = ps_tested_face_to_plane_normal_d_param[tested_faces_iter->first];
            int& tested_face_plane_normal_max_comp = ps_tested_face_to_plane_normal_max_comp[tested_faces_iter->first];

            tested_face_plane_normal_max_comp = compute_polygon_plane_coefficients(
                tested_face_plane_normal,
                tested_face_plane_param_d,
                tested_face_vertices.data(),
                (int)tested_face_vertices.size());

            if (squared_length(tested_face_plane_normal) == 0) {
                const int tmp_local = (int)tested_faces_iter->first;
                const bool is_cutmesh_face = (tmp_local > sm_face_count);
                const std::string msh_name = is_cutmesh_face ? "cut-mesh" : "source-mesh";
                const fd_t bad_face_desr = fd_t(is_cutmesh_face ? (tmp_local - sm_face_count) : tmp_local);
                lg.set_reason_for_failure("face f" + std::to_string(bad_face_desr) + " of " + msh_name + " is degenerate (has zero area)");
                output.status = (is_cutmesh_face ? status_t::INVALID_CUT_MESH : status_t::INVALID_SRC_MESH);
                return;
            }
        }
    }
#endif
    TIMESTACK_POP();

    // edge-to-face intersection tests (narrow-phase)
    // -----------------------------------------

    // ivertex to faces that meet at the this ivertex
    // std::map<
    ///    vd_t, // intersection point
    //    std::vector<fd_t> // list of faces that intersect with another face at the intersection point
    //    >
    //    m0_ivtx_to_ps_faces;

    // ivertex to halfedge that was tested again a face in order to produce this ivertex
    // NOTE: the ordering of the intersections point is dependant on the order in which they where actually computed
    // i.e. the data of the first intersection point is at index 0
    // std::map<
    //    vd_t, // intersection point
    //    ed_t // halfedge
    //    >
    //    m0_ivtx_to_ps_edge;

    std::vector<
        std::pair<ed_t, fd_t> // edge and face that where tested to produce our intersection point.
        >
        m0_ivtx_to_intersection_registry_entry;

    // re-entrant vertices on the border of the cut-mesh
    std::vector<vd_t> cm_border_reentrant_ivtx_list;

    // std::map<
    ///    vd_t, // intersection point
    ///    vec3 // the normal vector of intersected face from which intersection point came from
    //    >
    //    m0_ivtx_to_tested_polygon_normal;

    // edges of the polygon soup mesh which intersect a face
    std::unordered_map<ed_t, std::vector<vd_t>> ps_intersecting_edges;

    // A map of used to create edges along the intersection path.
    // Each element is the information such as intersection points that arise from testing two polygons.
    // The size of thos vector is dependent on the number of polygon pairs (in "input.intersecting_sm_cm_face_pairs")
    // which intersect.

    std::map< // information needed to build edges along the cut-path
        pair<fd_t>, // pair of intersecting polygons (source-mesh polygon, cut-mesh polygon)
        std::vector<vd_t> // resulting intersection points
        >
        cutpath_edge_creation_info;

    std::unordered_map<
        fd_t, // intersectiong face
        std::vector<vd_t> // intersection point which involve the intersecting face
        >
        ps_iface_to_ivtx_list; // faces which intersect with another

    // A partial cut intersection exists when there exists at-least one intersection point
    // whose registry has a halfedge from the cut-surface, where this halfedge is a border halfedge.
    bool partial_cut_detected = false;

    TIMESTACK_PUSH("Calculate intersection points (edge-to-face)");

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        // typedef std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator InputStorageIteratorType;
        typedef std::tuple<
            std::vector<std::pair<ed_t, fd_t>>, // m0_ivtx_to_intersection_registry_entry
            std::vector<vd_t>, // cm_border_reentrant_ivtx_list
            std::unordered_map<ed_t, std::vector<vd_t>>, // ps_intersecting_edges
            std::map<pair<fd_t>, std::vector<vd_t>>, // cutpath_edge_creation_info
            std::unordered_map<fd_t, std::vector<vd_t>>, // ps_iface_to_ivtx_list
            bool, // partial_cut_detected
            std::vector<vec3> // list of intersection points computed in a future
            >
            OutputStorageTypesTuple;

        auto fn_compute_intersection_points = [&](
                                                  std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator block_start_,
                                                  std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator block_end_) -> OutputStorageTypesTuple {
            OutputStorageTypesTuple local_output;

            std::vector<std::pair<ed_t, fd_t>>& m0_ivtx_to_intersection_registry_entry_LOCAL = std::get<0>(local_output);
            std::vector<vd_t>& cm_border_reentrant_ivtx_list_LOCAL = std::get<1>(local_output);
            std::unordered_map<ed_t, std::vector<vd_t>>& ps_intersecting_edges_LOCAL = std::get<2>(local_output);
            std::map<pair<fd_t>, std::vector<vd_t>>& cutpath_edge_creation_info_LOCAL = std::get<3>(local_output);
            std::unordered_map<fd_t, std::vector<vd_t>>& ps_iface_to_ivtx_list_LOCAL = std::get<4>(local_output);
            bool& partial_cut_detected_LOCAL = std::get<5>(local_output);
            std::vector<vec3>& intersection_points_LOCAL = std::get<6>(local_output);

            // NOTE: threads do not add vertices into m0 to prevent contention on a shared resource.
            // They instead store a placeholder value that is analogous to an local offset i.e.
            // as if each thread was writing into its own mesh.

            // did another thread/job encountered a configuration in which we violated general position
            // NOTE: we only check at the beginning to prevent contention on "output.status"
            bool current_status_is_okay = (output.status.load() == status_t::SUCCESS);
            if (!current_status_is_okay) {
                return OutputStorageTypesTuple {}; // immediately
            }

            // for each edge
            for (std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator ps_edge_face_intersection_pairs_iter = block_start_;
                 ps_edge_face_intersection_pairs_iter != block_end_;
                 ps_edge_face_intersection_pairs_iter++) {

                // our edge that we test for intersection with other faces
                const ed_t tested_edge = ps_edge_face_intersection_pairs_iter->first;
                // the faces against which the edge is tested for intersection
                const std::vector<fd_t>& tested_faces = ps_edge_face_intersection_pairs_iter->second;

                // the halfedges of our edge
                const hd_t tested_edge_h0 = ps.halfedge(tested_edge, 0);
                const hd_t tested_edge_h1 = ps.halfedge(tested_edge, 1);

                // source vertex
                const vertex_descriptor_t tested_edge_h0_source_descr = ps.source(tested_edge_h0);
                const vec3& tested_edge_h0_source_vertex = ps.vertex(tested_edge_h0_source_descr);
                // target vertex
                const vertex_descriptor_t tested_edge_h0_target_descr = ps.target(tested_edge_h0);
                const vec3& tested_edge_h0_target_vertex = ps.vertex(tested_edge_h0_target_descr);

                // This boolean var is evaluated based on the fact that sm faces come before cm faces inside the "ps" data structure
                const fd_t tested_edge_h0_face = ps.face(tested_edge_h0);
                const fd_t tested_edge_h1_face = ps.face(tested_edge_h1);
                const fd_t tested_edge_face = tested_edge_h0_face != hmesh_t::null_face() ? tested_edge_h0_face : tested_edge_h1_face;
                const bool tested_edge_belongs_to_cm = ps_is_cutmesh_face(tested_edge_face, sm_face_count);

                // for each face that is to be intersected with the tested-edge
                for (std::vector<fd_t>::const_iterator tested_faces_iter = tested_faces.cbegin();
                     tested_faces_iter != tested_faces.cend();
                     ++tested_faces_iter) {
                    const fd_t tested_face = *tested_faces_iter;

                    // We are now finding the intersection points determined by calculating the location
                    // where each halfedge of face A intersects the area defined by face B (if it exists).

                    // get the vertices of tested_face (used to estimate its normal etc.)
                    MCUT_ASSERT(ps_tested_face_to_vertices.find(tested_face) != ps_tested_face_to_vertices.end());
                    const std::vector<vec3>& tested_face_vertices = SAFE_ACCESS(ps_tested_face_to_vertices, tested_face);

                    // compute plane of tested_face
                    // -----------------------

                    MCUT_ASSERT(ps_tested_face_to_plane_normal.find(tested_face) != ps_tested_face_to_plane_normal.end());
                    const vec3& tested_face_plane_normal = SAFE_ACCESS(ps_tested_face_to_plane_normal, tested_face);
                    MCUT_ASSERT(ps_tested_face_to_plane_normal_d_param.find(tested_face) != ps_tested_face_to_plane_normal_d_param.end());
                    const double& tested_face_plane_param_d = SAFE_ACCESS(ps_tested_face_to_plane_normal_d_param, tested_face);
                    MCUT_ASSERT(ps_tested_face_to_plane_normal_max_comp.find(tested_face) != ps_tested_face_to_plane_normal_max_comp.end());
                    const int& tested_face_plane_normal_max_comp = SAFE_ACCESS(ps_tested_face_to_plane_normal_max_comp, tested_face); // compute_polygon_plane_coefficients(

                    vec3 intersection_point(0., 0., 0.); // the intersection point to be computed

                    char segment_intersection_type = compute_segment_plane_intersection_type( // exact**
                        tested_edge_h0_source_vertex,
                        tested_edge_h0_target_vertex,
                        tested_face_vertices,
                        tested_face_plane_normal,
                        tested_face_plane_normal_max_comp);

                    bool have_plane_intersection = (segment_intersection_type != '0'); // any intersection !

                    if (have_plane_intersection) {
                        if (segment_intersection_type != '1') {
                            bool violatedGP = false;
                            std::vector<const vec3*> points_touching_plane;

                            if (segment_intersection_type == 'q' || segment_intersection_type == 'r') {
                                points_touching_plane.push_back((segment_intersection_type == 'q') ? &tested_edge_h0_source_vertex : &tested_edge_h0_target_vertex);
                            } else {
                                points_touching_plane.push_back(&tested_edge_h0_source_vertex);
                                points_touching_plane.push_back(&tested_edge_h0_target_vertex);
                            }

                            for (std::vector<const vec3*>::const_iterator i = points_touching_plane.cbegin(); i != points_touching_plane.cend(); ++i) {
                                const vec3& point = (*(*i));
                                char result = compute_point_in_polygon_test(
                                    point,
                                    tested_face_vertices,
                                    tested_face_plane_normal,
                                    tested_face_plane_normal_max_comp);
                                if (result == 'i' || (result == 'v' || result == 'e')) {
                                    violatedGP = true;
                                    break;
                                }
                            }

                            if (violatedGP) {
                                status_t okay_status = status_t::SUCCESS;
                                bool exchanged = output.status.compare_exchange_strong(okay_status, status_t::GENERAL_POSITION_VIOLATION);
                                if (!input.enforce_general_position && exchanged) {
                                    // only one thread can modify "lg". Thus, "lg" does not need to be locked in order to set
                                    // the reason for failure
                                    lg.set_reason_for_failure("invalid compute_segment_plane_intersection_type result ('" + std::to_string(segment_intersection_type) + "')");
                                }
                                return OutputStorageTypesTuple {};
                                ; // stop immediately and do not complete the current job
                            } else {
                                //  move onto the next edge-face test.
                                continue;
                            }
                        }

                        compute_segment_plane_intersection(
                            intersection_point,
                            tested_face_plane_normal,
                            tested_face_plane_param_d,
                            tested_edge_h0_source_vertex,
                            tested_edge_h0_target_vertex);

                        char in_poly_test_intersection_type = compute_point_in_polygon_test(
                            intersection_point,
                            tested_face_vertices,
                            // #if 1
                            tested_face_plane_normal,
                            // #else
                            tested_face_plane_normal_max_comp
                            // #endif
                        );

                        if (in_poly_test_intersection_type == 'v' || in_poly_test_intersection_type == 'e') {
                            status_t okay_status = status_t::SUCCESS;
                            bool exchanged = output.status.compare_exchange_strong(okay_status, status_t::GENERAL_POSITION_VIOLATION);
                            // output.status = status_t::GENERAL_POSITION_VIOLATION;
                            if (!input.enforce_general_position && exchanged) {
                                lg.set_reason_for_failure("invalid point-in-polygon test result ('" + std::to_string(in_poly_test_intersection_type) + "')");
                            }
                            return OutputStorageTypesTuple {};
                            ; // stop immediately and do not complete the current job
                        }

                        bool have_point_in_polygon = in_poly_test_intersection_type == 'i';

                        if (have_point_in_polygon) {

                            fd_t face_pqr = tested_edge_face;
                            fd_t face_xyz = tested_face;
                            fd_t face_pqs = tested_edge_face == tested_edge_h0_face ? tested_edge_h1_face : hmesh_t::null_face();

                            vd_t new_vertex_descr((vd_t::index_type)intersection_points_LOCAL.size());
                            intersection_points_LOCAL.push_back(intersection_point); /*m0.add_vertex(intersection_point)*/
                            ;

                            MCUT_ASSERT((size_t)new_vertex_descr == m0_ivtx_to_intersection_registry_entry_LOCAL.size() /*m0_ivtx_to_intersection_registry_entry.find(new_vertex_descr) == m0_ivtx_to_intersection_registry_entry.cend()*/);
                            m0_ivtx_to_intersection_registry_entry_LOCAL.push_back(std::make_pair(tested_edge, tested_face));

                            ps_intersecting_edges_LOCAL[tested_edge].push_back(new_vertex_descr);

                            const fd_t cm_face = tested_edge_belongs_to_cm ? tested_edge_face : tested_face;
                            const fd_t sm_face = tested_edge_belongs_to_cm ? tested_face : tested_edge_face;

                            if (tested_edge_belongs_to_cm) {
                                // NOTE: std::pair format/order is {source-mesh-face, cut-mesh-face}
                                cutpath_edge_creation_info_LOCAL[make_pair(tested_face, face_pqr)].push_back(new_vertex_descr);
                                if (face_pqs != hmesh_t::null_face()) {
                                    cutpath_edge_creation_info_LOCAL[make_pair(tested_face, face_pqs)].push_back(new_vertex_descr);
                                }
                            } else {
                                cutpath_edge_creation_info_LOCAL[make_pair(tested_edge_face, tested_face)].push_back(new_vertex_descr);
                                const fd_t tested_edge_face_other = (tested_edge_face == tested_edge_h0_face) ? tested_edge_h1_face : tested_edge_h0_face;

                                if (tested_edge_face_other != hmesh_t::null_face()) {
                                    cutpath_edge_creation_info_LOCAL[make_pair(tested_edge_face_other, tested_face)].push_back(new_vertex_descr);
                                }
                            }

                            if (tested_edge_belongs_to_cm) {
                                const bool is_border_reentrant_ivertex = ps.is_border(tested_edge); // ps.is_border(ps.edge(halfedge_pq));

                                if (is_border_reentrant_ivertex) {
                                    cm_border_reentrant_ivtx_list_LOCAL.push_back(new_vertex_descr);
                                }
                            }

                            ps_iface_to_ivtx_list_LOCAL[tested_face].push_back(new_vertex_descr);
                            if (tested_edge_h0_face != hmesh_t::null_face()) {
                                ps_iface_to_ivtx_list_LOCAL[tested_edge_h0_face].push_back(new_vertex_descr);
                            }
                            if (tested_edge_h1_face != hmesh_t::null_face()) {
                                ps_iface_to_ivtx_list_LOCAL[tested_edge_h1_face].push_back(new_vertex_descr);
                            }

                            if (partial_cut_detected_LOCAL == false) { // keep checking until (locally) true
                                const bool is_cs_edge = ps_is_cutmesh_vertex(tested_edge_h0_source_descr, sm_vtx_cnt);
                                bool is_border = (tested_edge_h0_face == hmesh_t::null_face() || tested_edge_h1_face == hmesh_t::null_face());
                                partial_cut_detected_LOCAL = (is_cs_edge && is_border);
                            }
                        } // if (have_point_in_polygon)
                    } // if (have_plane_intersection) {
                } // for (std::vector<fd_t>::const_iterator intersected_faces_iter = intersected_faces.cbegin(); intersected_faces_iter != intersected_faces.cend(); ++intersected_faces_iter) {
            } // for (std::map<ed_t, std::vector<fd_t>>::const_iterator ps_edge_face_intersection_pairs_iter = ps_edge_face_intersection_pairs.cbegin(); ps_edge_face_intersection_pairs_iter != ps_edge_face_intersection_pairs.cend(); ps_edge_face_intersection_pairs_iter++) {

            return local_output;
        };

        std::vector<std::future<OutputStorageTypesTuple>> futures;
        OutputStorageTypesTuple partial_res;
        parallel_for(
            *input.scheduler,
            ps_edge_face_intersection_pairs.cbegin(),
            ps_edge_face_intersection_pairs.cend(),
            fn_compute_intersection_points,
            partial_res, // output computed by master thread
            futures);

        std::vector<vec3> intersection_points;
        std::tie(
            m0_ivtx_to_intersection_registry_entry,
            cm_border_reentrant_ivtx_list,
            ps_intersecting_edges,
            cutpath_edge_creation_info,
            ps_iface_to_ivtx_list,
            partial_cut_detected,
            intersection_points)
            = partial_res;

        // Add intersection points computed by master thread in to "m0" and
        // account for intersection point offsets
        for (std::vector<vec3>::const_iterator i = intersection_points.cbegin(); i != intersection_points.cend(); ++i) {
            const vd_t stored_descr = m0.add_vertex(*i);
            MCUT_ASSERT(stored_descr != hmesh_t::null_vertex());
        }

        for (int i = 0; i < (int)cm_border_reentrant_ivtx_list.size(); ++i) {
            cm_border_reentrant_ivtx_list[i] += ps_vtx_cnt;
        }

        for (std::unordered_map<ed_t, std::vector<vd_t>>::iterator i = ps_intersecting_edges.begin();
             i != ps_intersecting_edges.end(); ++i) {
            for (std::vector<vd_t>::iterator j = i->second.begin(); j != i->second.end(); j++) {
                *j += ps_vtx_cnt;
            }
        }

        for (std::unordered_map<fd_t, std::vector<vd_t>>::iterator i = ps_iface_to_ivtx_list.begin();
             i != ps_iface_to_ivtx_list.end(); ++i) {
            for (std::vector<vd_t>::iterator j = i->second.begin(); j != i->second.end(); j++) {
                *j += ps_vtx_cnt;
            }
        }

        for (std::map<pair<fd_t>, std::vector<vd_t>>::iterator i = cutpath_edge_creation_info.begin();
             i != cutpath_edge_creation_info.end(); ++i) {
            for (std::vector<vd_t>::iterator j = i->second.begin(); j != i->second.end(); j++) {
                *j += ps_vtx_cnt;
            }
        }

        //
        // Now we merge the results from futures

        bool status_is_okay = true;

        if (futures.empty()) { // only master thred
            status_is_okay = (output.status.load() == status_t::SUCCESS);
            if (!status_is_okay) {
                return;
            }
        } else { // one or more worker threads

            //
            // This scope is executed by the master thread, which will merge results from other threads via std::futures
            //
            vd_t intersection_point_descr_baseoffset(m0.number_of_vertices());

            // iterate through all available future so that we can 1) get their results
            // and 2) ensure that all scheduled jobs are completed before exiting (or returning from)
            // the kernel
            for (int i_ = 0; i_ < (int)futures.size(); ++i_) {

                std::future<OutputStorageTypesTuple>& f = futures[i_];
                MCUT_ASSERT(f.valid()); // The behavior is undefined if valid()== false before the call to wait_for

                OutputStorageTypesTuple future_result = f.get(); // "get()" is a blocking function

                // As the master thread works to merge the partial results, it is possible that one of the
                // worker threads detected a violation of general position. In this case, the current dispatch call
                // needs to be stopped so that the front-end will be able to perturb the cut-mesh.
                //
                // Thus, we ask "did any worker-thread encounter a GP violation?" If so, we must stop all merging
                // of partial results [and] wait for the currently running jobs (futures) to finish. Waiting is
                // done automatically by calling f.get()
                status_is_okay = (output.status.load() == status_t::SUCCESS);

                if (!status_is_okay) {
                    continue; // skip future result
                }

                const std::vector<std::pair<ed_t, fd_t>>& m0_ivtx_to_intersection_registry_entry_FUTURE = std::get<0>(future_result);
                const std::vector<vd_t>& cm_border_reentrant_ivtx_list_FUTURE = std::get<1>(future_result);
                const std::unordered_map<ed_t, std::vector<vd_t>>& ps_intersecting_edges_FUTURE = std::get<2>(future_result);
                const std::map<pair<fd_t>, std::vector<vd_t>>& cutpath_edge_creation_info_FUTURE = std::get<3>(future_result);
                const std::unordered_map<fd_t, std::vector<vd_t>>& ps_iface_to_ivtx_list_FUTURE = std::get<4>(future_result);
                const bool& partial_cut_detected_FUTURE = std::get<5>(future_result);
                const std::vector<vec3>& intersection_points_FUTURE = std::get<6>(future_result);
                const uint32_t intersection_points_in_future = (uint32_t)intersection_points_FUTURE.size();

                MCUT_ASSERT(intersection_points_FUTURE.size() == m0_ivtx_to_intersection_registry_entry_FUTURE.size());

                // add intersection point corresponding to the current future
                for (std::vector<vec3>::const_iterator it = intersection_points_FUTURE.cbegin();
                     it != intersection_points_FUTURE.cend();
                     ++it) {
                    const vd_t stored_descr = m0.add_vertex(*it);
                    MCUT_ASSERT(stored_descr != hmesh_t::null_vertex());
                }

                // merge m0_ivtx_to_intersection_registry_entry_FUTURE
                m0_ivtx_to_intersection_registry_entry.insert(
                    m0_ivtx_to_intersection_registry_entry.end(),
                    m0_ivtx_to_intersection_registry_entry_FUTURE.cbegin(),
                    m0_ivtx_to_intersection_registry_entry_FUTURE.cend());

                // merge cm_border_reentrant_ivtx_list_FUTURE
                for (std::vector<vd_t>::const_iterator it = cm_border_reentrant_ivtx_list_FUTURE.cbegin();
                     it != cm_border_reentrant_ivtx_list_FUTURE.cend();
                     ++it) {
                    const vd_t rel_descr = (*it);
                    const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                    cm_border_reentrant_ivtx_list.push_back(actual_descr);
                }

                // merge ps_intersecting_edges_FUTURE
                for (std::unordered_map<ed_t, std::vector<vd_t>>::const_iterator i = ps_intersecting_edges_FUTURE.cbegin();
                     i != ps_intersecting_edges_FUTURE.cend();
                     ++i) {
                    std::unordered_map<ed_t, std::vector<vd_t>>::iterator fiter = ps_intersecting_edges.find(i->first);
                    bool collision = (fiter != ps_intersecting_edges.end());
                    if (collision) { // another thread also encounter the same edge
                        for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                            const vd_t rel_descr = (*j);
                            const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                            if (std::find(fiter->second.cbegin(), fiter->second.cend(), *j) == fiter->second.cend()) {
                                fiter->second.push_back(actual_descr);
                            }
                        }
                    } else {
                        // just insert and account for offset
                        for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                            const vd_t rel_descr = (*j);
                            const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                            ps_intersecting_edges[i->first].push_back(actual_descr);
                        }
                    }
                }

                // merge cutpath_edge_creation_info_FUTURE
                for (std::map<pair<fd_t>, std::vector<vd_t>>::const_iterator i = cutpath_edge_creation_info_FUTURE.cbegin();
                     i != cutpath_edge_creation_info_FUTURE.cend();
                     ++i) {
                    std::map<pair<fd_t>, std::vector<vd_t>>::iterator fiter = cutpath_edge_creation_info.find(i->first);
                    bool collision = (fiter != cutpath_edge_creation_info.end());
                    if (collision) { // another thread also encounter the same key
                        for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                            const vd_t rel_descr = (*j);
                            const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                            if (std::find(fiter->second.cbegin(), fiter->second.cend(), actual_descr) == fiter->second.cend()) {
                                fiter->second.push_back(actual_descr);
                            }
                        }
                    } else {
                        // just insert and account for offset
                        for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                            const vd_t rel_descr = (*j);
                            const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                            cutpath_edge_creation_info[i->first].push_back(actual_descr);
                        }
                    }
                }

                // merge ps_iface_to_ivtx_list_FUTURE
                for (std::unordered_map<fd_t, std::vector<vd_t>>::const_iterator i = ps_iface_to_ivtx_list_FUTURE.cbegin();
                     i != ps_iface_to_ivtx_list_FUTURE.cend();
                     ++i) {
                    std::unordered_map<fd_t, std::vector<vd_t>>::iterator fiter = ps_iface_to_ivtx_list.find(i->first);
                    bool collision = (fiter != ps_iface_to_ivtx_list.end());
                    if (collision) { // another thread also encounter the same face
                        for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                            const vd_t rel_descr = (*j);
                            const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                            if (std::find(fiter->second.cbegin(), fiter->second.cend(), actual_descr) == fiter->second.cend()) {
                                fiter->second.push_back(actual_descr);
                            }
                        }
                    } else {
                        // just insert and account for offset
                        for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
                            const vd_t rel_descr = (*j);
                            const vd_t actual_descr = vd_t(intersection_point_descr_baseoffset + rel_descr);
                            ps_iface_to_ivtx_list[i->first].push_back(actual_descr);
                        }
                    }
                }

                // merge (i.e. boolean-wise OR) partial_cut_detected_FUTURE
                partial_cut_detected = (partial_cut_detected || partial_cut_detected_FUTURE) ? true : false;

                // shift to account for the (number of) intersection points computed in the current future
                intersection_point_descr_baseoffset += intersection_points_in_future;
            } // while(!futures.empty()) {
        } // if(num_blocks > 1)

        if (!status_is_okay) {
            // Safely return to the front-end since all jobs are now successively finished/cancelled.
            //
            // CAUTION: if the master thread returns before all current jobs are done (e.g.
            // successively cancelled) the worker threads would end up accessing dangling
            // references to e.g. variables like "ps", "m0", etc.
            return;
        }
    } // end of parallel execution scope
#else
    for (std::unordered_map<ed_t, std::vector<fd_t>>::const_iterator ps_edge_face_intersection_pairs_iter = ps_edge_face_intersection_pairs.cbegin();
         ps_edge_face_intersection_pairs_iter != ps_edge_face_intersection_pairs.cend();
         ps_edge_face_intersection_pairs_iter++) {

        // our edge that we test for intersection with other faces
        const ed_t tested_edge = ps_edge_face_intersection_pairs_iter->first;
        // the faces against which the edge is tested for intersection
        const std::vector<fd_t>& tested_faces = ps_edge_face_intersection_pairs_iter->second;

        // the halfedges of our edge
        const hd_t tested_edge_h0 = ps.halfedge(tested_edge, 0);
        const hd_t tested_edge_h1 = ps.halfedge(tested_edge, 1);

        // source vertex
        const vertex_descriptor_t tested_edge_h0_source_descr = ps.source(tested_edge_h0);
        const vec3& tested_edge_h0_source_vertex = ps.vertex(tested_edge_h0_source_descr);
        // target vertex
        const vertex_descriptor_t tested_edge_h0_target_descr = ps.target(tested_edge_h0);
        const vec3& tested_edge_h0_target_vertex = ps.vertex(tested_edge_h0_target_descr);

        // This boolean var is evaluated based on the fact that sm faces come before cm faces inside the "ps" data structure
        const fd_t tested_edge_h0_face = ps.face(tested_edge_h0);
        const fd_t tested_edge_h1_face = ps.face(tested_edge_h1);
        const fd_t tested_edge_face = tested_edge_h0_face != hmesh_t::null_face() ? tested_edge_h0_face : tested_edge_h1_face;
        const bool tested_edge_belongs_to_cm = ps_is_cutmesh_face(tested_edge_face, sm_face_count);

        // for each face that is to be intersected with the tested-edge
        for (std::vector<fd_t>::const_iterator tested_faces_iter = tested_faces.cbegin();
             tested_faces_iter != tested_faces.cend();
             ++tested_faces_iter) {
            const fd_t tested_face = *tested_faces_iter;

            // We are now finding the intersection points determined by calculating the location
            // where each halfedge of face A intersects the area defined by face B (if it exists).

            // get the vertices of tested_face (used to estimate its normal etc.)
            // std::vector<vd_t> tested_face_descriptors = ps.get_vertices_around_face(tested_face);
            MCUT_ASSERT(ps_tested_face_to_vertices.find(tested_face) != ps_tested_face_to_vertices.end());
            const std::vector<vec3>& tested_face_vertices = SAFE_ACCESS(ps_tested_face_to_vertices, tested_face);

            // compute plane of tested_face
            // -----------------------

            MCUT_ASSERT(ps_tested_face_to_plane_normal.find(tested_face) != ps_tested_face_to_plane_normal.end());
            const vec3& tested_face_plane_normal = SAFE_ACCESS(ps_tested_face_to_plane_normal, tested_face);
            MCUT_ASSERT(ps_tested_face_to_plane_normal_d_param.find(tested_face) != ps_tested_face_to_plane_normal_d_param.end());
            const double& tested_face_plane_param_d = SAFE_ACCESS(ps_tested_face_to_plane_normal_d_param, tested_face);
            MCUT_ASSERT(ps_tested_face_to_plane_normal_max_comp.find(tested_face) != ps_tested_face_to_plane_normal_max_comp.end());
            const int& tested_face_plane_normal_max_comp = SAFE_ACCESS(ps_tested_face_to_plane_normal_max_comp, tested_face);

            vec3 intersection_point(0., 0., 0.); // the intersection po int to be computed

            // TODO: replace this with shewchuck predicate (nasty failure on test 42)
            // at least orient3d will be able to give use the corrent result!
#if 0
            char lp_intersection_result = compute_line_plane_intersection(
                intersection_point,
                tested_edge_h0_source_vertex,
                tested_edge_h0_target_vertex,
                tested_face_vertices.data(),
                tested_face_vertices.size(),
                tested_face_plane_normal_max_comp,
                tested_face_plane_normal,
                tested_face_plane_param_d);
#else
            char segment_intersection_type = compute_segment_plane_intersection_type( // exact**
                tested_edge_h0_source_vertex,
                tested_edge_h0_target_vertex,
                tested_face_vertices,
                tested_face_plane_normal,
                tested_face_plane_normal_max_comp);
#endif
            bool have_plane_intersection = (segment_intersection_type != '0'); // any intersection !

            if (have_plane_intersection) { // does the segment intersect the plane?

                if (segment_intersection_type != '1') { // the segment only touches the the plane (the line reprsented by segment still intersects the plane)

                    // before jumping-the-gun and assuming that we have indeed violated GP,
                    // we should check whether the point found to be on the plane (touching point) is
                    // actually [inside] the tested_face. That would imply cutting through a vertex or edge (which is undefined).
                    // If this is true then we have indeed violated GP. Otherwise, we just treat this as a non-intersection because
                    // the what-would-have-been intersection point actually lies outside the tested_face.
                    bool violatedGP = false;
                    std::vector<const vec3*> points_touching_plane;

                    if (segment_intersection_type == 'q' /*segment start*/ || segment_intersection_type == 'r' /*segment end*/) { // only one segment end is touching plane
                        points_touching_plane.push_back((segment_intersection_type == 'q') ? &tested_edge_h0_source_vertex : &tested_edge_h0_target_vertex);
                    } else { // both point are in the plane, so we will need to confirm whether BOTH are outside our tested face.
                        points_touching_plane.push_back(&tested_edge_h0_source_vertex);
                        points_touching_plane.push_back(&tested_edge_h0_target_vertex);
                    }

                    // if any point in "points_touching_plane" is inside the tested_face then we have violated GP
                    for (std::vector<const vec3*>::const_iterator i = points_touching_plane.cbegin(); i != points_touching_plane.cend(); ++i) {
                        const vec3& point = (*(*i));
                        char result = compute_point_in_polygon_test(
                            point,
                            tested_face_vertices,
                            tested_face_plane_normal,
                            tested_face_plane_normal_max_comp);
                        if (
                            // the touching point is inside, which implies cutting through a vertex (of "tested_edge")
                            result == 'i' ||
                            // The following condition means that we will have an edge-edge intersection anyway!
                            // i.e. 'v' means that two edges (from sm and cm) touch at their tips/points since point is an end point of "tested_edge"
                            // ... and 'e' means that an end point of "tested_edge" touches an edge of "tested_face"
                            (result == 'v' || result == 'e')) {
                            violatedGP = true;
                            break;
                        }
                    }

                    if (violatedGP) {
                        output.status = status_t::GENERAL_POSITION_VIOLATION;
                        if (!input.enforce_general_position) {
                            // Our assumption of having inputs in general position has been violated, we need to terminate
                            // with an error since perturbation (enforcement of general positions) is disabled by the user.
                            // Note: our intersection registry formulation requires that edges completely penetrate/intersect through polygon's area.
                            lg.set_reason_for_failure("invalid compute_segment_plane_intersection_type result ('" + std::to_string(segment_intersection_type) + "')");
                        }
                        return; // bail and return to the front-end
                    } else {
                        // same as the case where "have_plane_intersection" is false.
                        // so we just move onto the next edge-face test.
                        continue;
                    }
                }

                // at this point, we have established that the segment actually intersects the plane [properly]

                // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
                // Now we compute the [actual] intersection point (coordinates)
                // and check whether it lies inside our polygon, or that GP has been violated,
                // which happens if e.g. the intersection point lies on an edge/vertex of "tested_face")

                // NOTE: if using fixed precision floats (i.e. double), then here we just care about getting the intersection point
                // irrespective of whether "segment_intersection_result" is consistent with "segment_intersection_type" from above.
                // The inconsistency can happen during edge cases. see e.g. test 42.
                compute_segment_plane_intersection(
                    intersection_point,
                    tested_face_plane_normal,
                    tested_face_plane_param_d,
                    tested_edge_h0_source_vertex,
                    tested_edge_h0_target_vertex);

                // is our intersection point in the polygon?
                char in_poly_test_intersection_type = compute_point_in_polygon_test(
                    intersection_point,
                    tested_face_vertices,
                    tested_face_plane_normal,
                    tested_face_plane_normal_max_comp);

                if (
                    // illegal on-edge and on-vertex intersections
                    (in_poly_test_intersection_type == 'v' || in_poly_test_intersection_type == 'e')) {
                    output.status = status_t::GENERAL_POSITION_VIOLATION;
                    if (!input.enforce_general_position) {
                        // Our assuption of having inputs in general position has been violated, we need to terminate
                        // with an error since perturbation (enforment of general positions) is disabled.
                        lg.set_reason_for_failure("invalid point-in-polygon test result ('" + std::to_string(in_poly_test_intersection_type) + "')");
                    }
                    return;
                }

                bool have_point_in_polygon = in_poly_test_intersection_type == 'i';

                if (have_point_in_polygon) { // NOTE: point must be [inside] the polygon for us to consider it further
#if 0
                    // Intersection point is now determined to be in side face-B (our polygon), now we must use the information
                    // we computed from the segment-plane intersection test Check source-mesh for defectsto find out if general position has been violated (i.e.
                    // invalid case of cutting through a vertex)
                    if (segment_intersection_type == 'p' || segment_intersection_type == 'q' || segment_intersection_type == 'r') {

                        output.status = status_t::GENERAL_POSITION_VIOLATION;

                        if (!input.enforce_general_position) {
                            // Our assumption of having inputs in general position has been violated, we need to terminate
                            // with an error since perturbation (i.e. enforcement of general positions) is disabled.
                            // If any one of a segment's vertices only touch (i.e. lie on) the plane
                            // then that implies a situation of cutting through a vertex which is undefined.
                            lg.set_reason_for_failure("segment-plane intersection ('" + std::to_string(segment_intersection_type) + "')");
                        }
                        return;
                    }
                    vd_t pre_existing_copy = hmesh_t::null_vertex(); // set to correct value if intersection has already been computed
#endif
                                             // The naming convention of these variables is based on Sifakis et al. 2007
                    // hd_t halfedge_pq = tested_edge_h0; // the halfedge which is intersected with polygon
                    // hd_t halfedge_pq_opp = tested_edge_h1; // ps.opposite(halfedge_pq);
                    fd_t face_pqr = tested_edge_face; // the face which is incident to halfedge-pq
                    fd_t face_xyz = tested_face; // the face which is intersected with halfedge-pq
                    fd_t face_pqs = tested_edge_face == tested_edge_h0_face ? tested_edge_h1_face : hmesh_t::null_face(); // ps.face(halfedge_pq_opp); // the face which is incident to the halfedge opposite to halfedge-pq
                                                                                                                          // fd_t face_pqX = hmesh_t::null_face(); // a virtual face pqX (where X denotes an unspecified auxiliary point)

#if 0
                    // add vertex if it does not exist.
                    // --------------------------------

                   
                    const bool pq_is_indicent_on_pqr_and_pqs = (face_pqs != hmesh_t::null_face()); // pq is common to faces pqr and pqs
                    std::vector<fd_t> new_vertex_incident_ps_faces; // the list of faces which are incident to our intersection point
                    // NOTE: Two intersection vertices are same if they are incident on the same faces AND their registry halfedges are opposites
                    //bool computed_intersection_point_exists = false;

                    if (pq_is_indicent_on_pqr_and_pqs) {

                        // all three faces are defined and meet at the intersection point
                        new_vertex_incident_ps_faces.push_back(face_pqr);
                        new_vertex_incident_ps_faces.push_back(face_pqs);
                        new_vertex_incident_ps_faces.push_back(face_xyz);

                    } // if (pq_is_indicent_on_pqr_and_pqs) {
                    else {
                        // pqr is the only face incident to pq
                        // -----------------------------------
                        new_vertex_incident_ps_faces.push_back(face_pqr);
                        new_vertex_incident_ps_faces.push_back(face_pqX); // virtual face
                        new_vertex_incident_ps_faces.push_back(face_xyz);
                    }
#endif
#if 0
                    int fv_count = 0;
                    const bool on_face  = point_on_face_plane(ps, tested_face, intersection_point, fv_count);
                    
                    if (!on_face)
                    { 
                        const vec3 normal = normalize(tested_face_plane_normal);
                        const double length = length(normal) ;

                        //MCUT_ASSERT(length == double(1.0));
                        const vec3& point_on_plane = tested_face_vertices.back(); // any vertex will do (assuming all vertices of face are coplanar)
                        const vec3 vec = (intersection_point - point_on_plane);
                        const double dot = dot_product(normal, vec);
                        intersection_point = intersection_point - (normal * dot);
                        point_on_face_plane(ps, tested_face, intersection_point, fv_count);
                    }
#endif
                    vd_t new_vertex_descr = m0.add_vertex(intersection_point);

#if 0

                    //
                    //
                    //
                    //
                    //
                    //
                    
                    //

#endif
                    // m0_ivtx_to_ps_faces.insert(std::make_pair(new_vertex_descr, new_vertex_incident_ps_faces));
                    // m0_ivtx_to_ps_edge.insert(std::make_pair(new_vertex_descr, tested_edge));
                    MCUT_ASSERT((size_t)new_vertex_descr - ps_vtx_cnt == m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(new_vertex_descr) == m0_ivtx_to_intersection_registry_entry.cend()*/);
                    m0_ivtx_to_intersection_registry_entry.push_back(std::make_pair(tested_edge, tested_face));

                    // ed_t e = ps.edge(halfedge_pq);
                    // bool edge_registered_as_intersecting = ps_intersecting_edges.find(tested_edge) != ps_intersecting_edges.cend();

                    ps_intersecting_edges[tested_edge].push_back(new_vertex_descr);

                    // intersection_test_ivtx_list.push_back(new_vertex_descr);

                    const fd_t cm_face = tested_edge_belongs_to_cm ? tested_edge_face : tested_face;
                    const fd_t sm_face = tested_edge_belongs_to_cm ? tested_face : tested_edge_face;

                    if (tested_edge_belongs_to_cm) {
                        // "tested_face" is from the source mesh

                        // NOTE: std::pair format/order is {source-mesh-face, cut-mesh-face}
                        cutpath_edge_creation_info[make_pair(tested_face, face_pqr)].push_back(new_vertex_descr);

                        if (face_pqs != hmesh_t::null_face()) {

                            cutpath_edge_creation_info[make_pair(tested_face, face_pqs)].push_back(new_vertex_descr);
                        }
                    } else {

                        cutpath_edge_creation_info[make_pair(tested_edge_face, tested_face)].push_back(new_vertex_descr);

                        const fd_t tested_edge_face_other = (tested_edge_face == tested_edge_h0_face) ? tested_edge_h1_face : tested_edge_h0_face;

                        if (tested_edge_face_other != hmesh_t::null_face()) {

                            cutpath_edge_creation_info[make_pair(tested_edge_face_other, tested_face)].push_back(new_vertex_descr);
                        }
                    }

                    // MCUT_ASSERT(m0_ivtx_to_tested_polygon_normal.count(new_vertex_descr) == 0);
                    // m0_ivtx_to_tested_polygon_normal[new_vertex_descr] = tested_face_plane_normal;
                    // MCUT_ASSERT(m0_ivtx_to_tested_polygon_normal.count(new_vertex_descr) == 1);

                    if (tested_edge_belongs_to_cm) { // halfedge_pq belongs to cut mesh

                        const bool is_border_reentrant_ivertex = ps.is_border(tested_edge); // ps.is_border(ps.edge(halfedge_pq));

                        if (is_border_reentrant_ivertex) {

                            cm_border_reentrant_ivtx_list.push_back(new_vertex_descr);
                        } // else // is regular
                    }

                    // map face to intersections points (reverse mapping of the intPoint-to-registryEntry)
                    ps_iface_to_ivtx_list[tested_face].push_back(new_vertex_descr);
                    if (tested_edge_h0_face != hmesh_t::null_face()) {
                        ps_iface_to_ivtx_list[tested_edge_h0_face].push_back(new_vertex_descr);
                    }
                    if (tested_edge_h1_face != hmesh_t::null_face()) {
                        ps_iface_to_ivtx_list[tested_edge_h1_face].push_back(new_vertex_descr);
                    }

                    if (partial_cut_detected == false) { // keep checking until true
                        // const vd_t v0 = ps.vertex(tested_edge, 0);
                        const bool is_cs_edge = ps_is_cutmesh_vertex(tested_edge_h0_source_descr, sm_vtx_cnt);
                        // partial_cut_detected = (is_cs_edge && ps.is_border(tested_edge));
                        bool is_border = (tested_edge_h0_face == hmesh_t::null_face() || tested_edge_h1_face == hmesh_t::null_face());
                        partial_cut_detected = (is_cs_edge && is_border);
                    }

                } // if (have_point_in_polygon)

            } // if (have_plane_intersection) {
        } // for (std::vector<fd_t>::const_iterator intersected_faces_iter = intersected_faces.cbegin(); intersected_faces_iter != intersected_faces.cend(); ++intersected_faces_iter) {

    } // for (std::map<ed_t, std::vector<fd_t>>::const_iterator ps_edge_face_intersection_pairs_iter = ps_edge_face_intersection_pairs.cbegin(); ps_edge_face_intersection_pairs_iter != ps_edge_face_intersection_pairs.cend(); ps_edge_face_intersection_pairs_iter++) {
#endif

    // Create edges from the new intersection points
    // ---------------------------------------------

    if (m0_ivtx_to_intersection_registry_entry.empty()) {
        lg.set_reason_for_failure("no face intersection found");
        if (input.enforce_general_position && input.general_position_enforcement_count > 0) {
            // This is not the first time we have invoked the kernel, which means that
            // perturbation pushed the cut-mesh into a state/position/configuration that
            // that does not actually intersect with the src-mesh. Thus, we will need to
            // perturb again!
            // By contruction, general position can only have been violated on inputs that
            // where intersecting (in some way) to begin with i.e. with the input meshes
            // as provided by the user. Thus, it would be incorrect to perturb the cut-mesh
            // into an intersection-free configuration and then claim that we have
            // successfully enforced general position. We want to make sure that after we
            // perturb we can at least produce *some* cut.
            output.status = status_t::GENERAL_POSITION_VIOLATION;
        } else {
            output.status = status_t::SUCCESS;
        }
        return;
    }

    TIMESTACK_POP();

#if 0
        for (std::map<vd_t, std::pair<ed_t, fd_t>>::const_iterator entry_it = m0_ivtx_to_intersection_registry_entry.cbegin(); entry_it != m0_ivtx_to_intersection_registry_entry.cend(); ++entry_it)
        {
            //const vd_t& ipoint_descr = entry_it->first;
            const ed_t &ipoint_iedge = entry_it->second.first;
            const vd_t v0 = ps.vertex(ipoint_iedge, 0);
            const bool is_cs_edge = ps_is_cutmesh_vertex(v0, sm_vtx_cnt);

            //if () {

            partial_cut_detected = (is_cs_edge && ps.is_border(ipoint_iedge));
            if (partial_cut_detected)
            {
                break;
            }

            // if (is_border) {
            //     partial_cut_detected = true;
            // }
            //}
        }
#endif

    if (partial_cut_detected && cm_border_reentrant_ivtx_list.size() == 0) {
        // can happen with case when both the input mesh and cut surface are not watertight
    }

    if (input.verbose) {
        dump_mesh(m0, "m0.v"); // containing only vertices (polygon soup vertices and newly computed intersection points)
    }

    if (partial_cut_detected) {

        MCUT_ASSERT(!cm_is_watertight);
        MCUT_ASSERT(!m0_ivtx_to_intersection_registry_entry.empty());
        // MCUT_ASSERT(!m0_ivtx_to_ps_edge.empty());

        if (input.require_looped_cutpaths) {
            output.status = status_t::SUCCESS;
            return;
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    // Check for degenerate mesh intersections
    ///////////////////////////////////////////////////////////////////////////

    // TODO: this is redundnat (remove)
    vertex_array_iterator_t m0_ivtx_iter_begin = m0.vertices_begin();
    std::advance(m0_ivtx_iter_begin, ps_vtx_cnt); // offset to start of intersection vertices in hmesh_t (note: internal mesh data stored consecutively)
#if 0
        //
        // check if at-least one source mesh edge intersects any face of the cut mesh.
        // ---------------------------------------------------------------------------

        /*for each intersection cs face
        for each halfedge of face        
            if halfedge intersects an im face and halfedge is a border halfedge
                1) find all other border halfedges of faces which also intersect im face
                if (1) > 0
                    check to ensure that at least one halfedge of the im face intersects the cs face 
    */

        // This check prevents malformed configurations where a cut-mesh face might stab/pierce
        // a face of the source-mesh face while correctly intersecting another source-mesh face
        // on the "other side". (Think of a wedge-like triangle stabbing a tet face while
        // intersecting [two] other faces by cutting an edge in the tet).
        bool atleast_one_sm_edge_intersects_an_cs_face = false;

        // TODO: just loop over m0_ivtx_to_intersection_registry_entry
        for (vertex_array_iterator_t i = m0_ivtx_iter_begin; i != m0.vertices_end(); ++i)
        {

            const ed_t &ps_edge = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (*i) - ps_vtx_cnt).first;
            const vd_t ps_edge_v0 = ps.vertex(ps_edge, 0);
            const bool is_sm_edge = !ps_is_cutmesh_vertex(ps_edge_v0, sm_vtx_cnt);

            if (is_sm_edge)
            {
                atleast_one_sm_edge_intersects_an_cs_face = true;
                break;
            }
        }

    if (!atleast_one_sm_edge_intersects_an_cs_face) {
        // NOTE: the sm must intersect at least one face of the cs to allow for an opening on the sm boundary.
        lg.set_reason_for_failure("found no edge in source mesh which intersects a cut mesh face.");
        output.status = status_t::INVALID_MESH_INTERSECTION;
        return;
    }
#endif
    ///////////////////////////////////////////////////////////////////////////
    // Create new edges along the intersection
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Create edges with intersection points "); // &&&&&

    // A mapping from an intersecting ps-face to the new edges. These edges are those whose
    // src and tgt vertices contain the respective face in their registry entry
    // (note: all or some may be used for used to clip the face).
    std::unordered_map<
        fd_t, // A face intersecting another
        std::vector<ed_t> // edges touching/incident on the intersecting face
        >
        ps_iface_to_m0_edge_list;

    // Edges defining the cut path/line of intersecton/intersection contour
    std::vector<ed_t> m0_cutpath_edges;

    // A mapping from and ivertex to the incoming halfedges
    std::unordered_map<
        vd_t, // intersection point
        std::vector<hd_t> // list of halfedges whose target is the intersection point
        >
        ivtx_to_incoming_hlist;
#if 0 // used for debugging colinearity bug, which occur when we have poly with eg. > 3
    // vertices where at least 3 more-or-less are colinear but exact predicate says no.
    for (std::map<pair<fd_t>, std::vector<vd_t>>::const_iterator cutpath_edge_creation_info_iter = cutpath_edge_creation_info.cbegin();
         cutpath_edge_creation_info_iter != cutpath_edge_creation_info.cend();
         ++cutpath_edge_creation_info_iter) {

        const fd_t sm_face = cutpath_edge_creation_info_iter->first.first;
        const fd_t cm_face = cutpath_edge_creation_info_iter->first.second;
        MCUT_ASSERT(!ps_is_cutmesh_face(sm_face, sm_face_count));
        const std::vector<vd_t>& intersection_test_ivtx_list = cutpath_edge_creation_info_iter->second;
        if ((int)intersection_test_ivtx_list.size() < 2) {
            const vd_t ivtx = intersection_test_ivtx_list.back();
            const ed_t& ps_edge = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, ivtx - ps_vtx_cnt).first;
            const fd_t ps_edge_f0 = ps.face(ps.halfedge(ps_edge, 0));
            const fd_t ps_edge_f1 = ps.face(ps.halfedge(ps_edge, 1));
            const fd_t ps_f = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, ivtx - ps_vtx_cnt).second;

#if 0
                auto dump_faces = [&](std::vector<fd_t> fv, std::string fpath)
                {
                    std::ofstream file(fpath);
                    for(vertex_array_iterator_t v = ps.vertices_begin(); v != ps.vertices_end(); ++v)
                    {
                        file << "v " << ps.vertex(*v).x() << " " << ps.vertex(*v).y() << " " << ps.vertex(*v).z() << std::endl;
                    }

                    for(auto f : fv)
                    {
                        file << "f ";
                        std::vector<vd_t> verts = ps.get_vertices_around_face(f); 
                        for(auto v : verts)
                        {
                            file << v+1 << " ";
                        }
                        file << "\n";
                    }
                };

                dump_faces({ ps_edge_f0, ps_edge_f1 }, "ps_edge.obj");
                dump_faces({ ps_f }, "ps_f.obj");

                dump_mesh(sm, "sm-.off");
                dump_mesh(cs, "cm-.off");
#endif

            output.status = status_t::GENERAL_POSITION_VIOLATION;

            if (!input.enforce_general_position) {

                // Our assumption of having inputs in general position has been violated, we need to terminate
                // with an error since perturbation (i.e. enforcement of general positions) is disabled.
                // If any one of a segment's vertices only touch (i.e. lie on) the plane
                // then that implies a situation of cutting through a vertex which is undefined.

                auto sm_or_cm = [&](fd_t f) {
                    return ps_is_cutmesh_face(f, sm_face_count) ? "cm" : "sm";
                };

                auto descr_v = [&](fd_t f) {
                    return ps_is_cutmesh_face(f, sm_face_count) ? f - sm_face_count : f;
                };

                char buff[128];
                sprintf(buff, "edge(%s.f%d, %s.f%d) lies exactly on face %s.f%d\n",
                    sm_or_cm(ps_edge_f0), (int)descr_v(ps_edge_f0),
                    sm_or_cm(ps_edge_f1), (int)descr_v(ps_edge_f1),
                    sm_or_cm(ps_f), (int)descr_v(ps_f));

                lg.set_reason_for_failure(buff);
            }

            return;
        }
    }
#endif
    for (std::map<pair<fd_t>, std::vector<vd_t>>::const_iterator cutpath_edge_creation_info_iter = cutpath_edge_creation_info.cbegin();
         cutpath_edge_creation_info_iter != cutpath_edge_creation_info.cend();
         ++cutpath_edge_creation_info_iter) {

        const fd_t sm_face = cutpath_edge_creation_info_iter->first.first;
        const fd_t cm_face = cutpath_edge_creation_info_iter->first.second;
        MCUT_ASSERT(!ps_is_cutmesh_face(sm_face, sm_face_count));
        const std::vector<vd_t>& intersection_test_ivtx_list = cutpath_edge_creation_info_iter->second;
        MCUT_ASSERT((int)intersection_test_ivtx_list.size() >= 2); // edge-case scenario: an edge intersects with another edge exactly
        const uint32_t new_ivertices_count = (uint32_t)intersection_test_ivtx_list.size();

        if (new_ivertices_count == 2) { // one edge
            vd_t first_new_ivertex = intersection_test_ivtx_list.front();
            vd_t second_new_ivertex = intersection_test_ivtx_list.back();

            MCUT_ASSERT(m0_is_intersection_point(first_new_ivertex, ps_vtx_cnt));
            MCUT_ASSERT(m0_is_intersection_point(second_new_ivertex, ps_vtx_cnt));

            bool cutpath_edge_exists = m0.halfedge(first_new_ivertex, second_new_ivertex, true) != hmesh_t::null_halfedge();
            if (
                cutpath_edge_exists == false
                //! interior_edge_exists(m0, first_new_ivertex, second_new_ivertex /*, m0_cutpath_edges*/)
            ) {

                hd_t h = m0.add_edge(first_new_ivertex, second_new_ivertex);
                MCUT_ASSERT(h != hmesh_t::null_halfedge());

                m0_cutpath_edges.emplace_back(m0.edge(h));

                // all newly created edges will lie on both face A and face B since intersection
                // points lie on a line which is the intersection of the two planes of face A and B
                ps_iface_to_m0_edge_list[sm_face].emplace_back(m0_cutpath_edges.back());
                ps_iface_to_m0_edge_list[cm_face].emplace_back(m0_cutpath_edges.back());

                update_neighouring_ps_iface_m0_edge_list(
                    first_new_ivertex,
                    second_new_ivertex,
                    ps,
                    sm_face,
                    cm_face,
                    m0_ivtx_to_intersection_registry_entry,
                    ps_iface_to_m0_edge_list,
                    m0_cutpath_edges);

                MCUT_ASSERT(m0.target(h) == second_new_ivertex);
                ivtx_to_incoming_hlist[second_new_ivertex].push_back(h);

                MCUT_ASSERT(m0.target(m0.opposite(h)) == first_new_ivertex);
                ivtx_to_incoming_hlist[first_new_ivertex].push_back(m0.opposite(h));
            }
        } else if (new_ivertices_count > 2) { // create N edges (N >= 1)

            // our produced intersection points
            std::vector<
                std::pair<
                    vd_t, // descriptor
                    vec3 // coordinates
                    >>
                ivertex_coords;

            for (uint32_t v = 0; v < new_ivertices_count; ++v) {
                vd_t new_ivertex_descr = intersection_test_ivtx_list[v];
                MCUT_ASSERT(m0_is_intersection_point(new_ivertex_descr, ps_vtx_cnt));
                ivertex_coords.emplace_back(new_ivertex_descr, m0.vertex(new_ivertex_descr));
            }

            std::vector<vd_t> sorted_descriptors = linear_projection_sort(ivertex_coords);

            // for (std::vector<std::pair<vd_t, vec3>>::const_iterator iter = ivertex_coords.cbegin() + 1; iter != ivertex_coords.cend(); ++iter) {
            for (std::vector<vd_t>::const_iterator iter = sorted_descriptors.cbegin() + 1; iter != sorted_descriptors.cend(); ++iter) {
                // const vd_t src_vertex = (iter - 1)->first;
                // const vd_t tgt_vertex = (iter)->first;
                const vd_t src_vertex = *(iter - 1);
                const vd_t tgt_vertex = *(iter);

                bool cutpath_edge_exists = m0.halfedge(src_vertex, tgt_vertex, true) != hmesh_t::null_halfedge();

                if (cutpath_edge_exists == false) {

                    // Here we also check whether the edge actually lies on the area of two [shared polygons]
                    // in the registry entries of its vertices. This operation is fundamentally geometric
                    // and cannot be resolved using topology (we have insufficient information to identify
                    // intersection edges). See benchmark test 34
                    // Thus, we will not add the edge if its mid-point does not lie in the area of [two]
                    // of the shared faces in the resgistry entries of its vertices (intersection points)..
                    // std::vector<std::pair<ed_t, fd_t>>::const_iterator find_iter = m0_ivtx_to_intersection_registry_entry.cend();

                    // get intersection-registry faces of src vertex
                    // find_iter = m0_ivtx_to_intersection_registry_entry.find(src_vertex);
                    MCUT_ASSERT((size_t)src_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*find_iter != m0_ivtx_to_intersection_registry_entry.cend()*/);
                    const std::vector<fd_t> src_vertex_faces = ps_get_ivtx_registry_entry_faces(ps, SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)src_vertex - ps_vtx_cnt) /*find_iter->second*/);

                    // get intersection-registry faces of tgt vertex
                    // find_iter = m0_ivtx_to_intersection_registry_entry.find(tgt_vertex);
                    MCUT_ASSERT((size_t)tgt_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*find_iter != m0_ivtx_to_intersection_registry_entry.cend()*/);
                    const std::vector<fd_t> tgt_vertex_faces = ps_get_ivtx_registry_entry_faces(ps, SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)tgt_vertex - ps_vtx_cnt) /*find_iter->second*/);

                    std::vector<fd_t> shared_faces;
                    std::copy_if(src_vertex_faces.begin(), src_vertex_faces.end(), std::back_inserter(shared_faces),
                        [&](fd_t f) {
                            return !is_virtual_face(f) && std::find(tgt_vertex_faces.cbegin(), tgt_vertex_faces.cend(), f) != tgt_vertex_faces.cend();
                        });

                    MCUT_ASSERT(shared_faces.size() >= 2); // connectable intersection points must match by 2 or more faces

                    // compute edge mid-point (could be any point along the edge that is not one of the vertices)
                    const vec3& src_vertex_coords = m0.vertex(src_vertex);
                    const vec3& tgt_vertex_coords = m0.vertex(tgt_vertex);
                    const vec3 midpoint = (tgt_vertex_coords + src_vertex_coords) * double(0.5);

                    std::vector<int> shared_faces_containing_edge;
                    // for each shared face
                    for (std::vector<fd_t>::const_iterator sf_iter = shared_faces.cbegin(); sf_iter != shared_faces.cend(); ++sf_iter) {
                        const fd_t shared_face = *sf_iter;

                        MCUT_ASSERT(ps_tested_face_to_plane_normal.find(shared_face) != ps_tested_face_to_plane_normal.cend());
                        const vec3& shared_face_plane_normal = SAFE_ACCESS(ps_tested_face_to_plane_normal, shared_face);

                        MCUT_ASSERT(ps_tested_face_to_plane_normal_max_comp.find(shared_face) != ps_tested_face_to_plane_normal_max_comp.cend());
                        int shared_face_normal_max_comp = SAFE_ACCESS(ps_tested_face_to_plane_normal_max_comp, shared_face);

                        MCUT_ASSERT(ps_tested_face_to_vertices.find(shared_face) != ps_tested_face_to_vertices.cend());
                        const std::vector<vec3>& shared_face_vertices = SAFE_ACCESS(ps_tested_face_to_vertices, shared_face);

                        char in_poly_test_intersection_type = compute_point_in_polygon_test(
                            midpoint,
                            shared_face_vertices,
                            shared_face_plane_normal,
                            shared_face_normal_max_comp);

                        if (in_poly_test_intersection_type == 'i') {
                            const int idx = (int)std::distance(shared_faces.cbegin(), sf_iter);
                            shared_faces_containing_edge.push_back(idx);
                        }
                    }

                    MCUT_ASSERT((int)shared_faces_containing_edge.size() <= 2);

                    if ((int)shared_faces_containing_edge.size() == 2) {

                        const hd_t h = m0.add_edge(src_vertex, tgt_vertex); // insert segment!

                        MCUT_ASSERT(h != hmesh_t::null_halfedge());
                        m0_cutpath_edges.emplace_back(m0.edge(h));

                        // NOTE: here we add all edge without assuming anything about which of the will be used to clip either polygon
                        // ps_iface_to_m0_edge_list[sm_face].emplace_back(m0_cutpath_edges.back());
                        // ps_iface_to_m0_edge_list[cm_face].emplace_back(m0_cutpath_edges.back());
                        for (std::vector<int>::const_iterator i = shared_faces_containing_edge.cbegin(); i != shared_faces_containing_edge.cend(); ++i) {
                            const fd_t shared_face = SAFE_ACCESS(shared_faces, *i);
                            ps_iface_to_m0_edge_list[shared_face].emplace_back(m0_cutpath_edges.back());
                        }

                        // update_neighouring_ps_iface_m0_edge_list(src_vertex, tgt_vertex, ps,
                        //     sm_face,
                        //     cm_face,
                        //     m0_ivtx_to_intersection_registry_entry,
                        //     ps_iface_to_m0_edge_list,
                        //     m0_cutpath_edges);

                        MCUT_ASSERT(m0.target(h) == tgt_vertex);
                        ivtx_to_incoming_hlist[tgt_vertex].push_back(h);

                        MCUT_ASSERT(m0.target(m0.opposite(h)) == src_vertex);
                        ivtx_to_incoming_hlist[src_vertex].push_back(m0.opposite(h));
                    }
                }
            }
        } // else if (new_ivertices_count > 2) {
    }

    TIMESTACK_POP();

    // NOTE: at this stage we have all vertices and edges which are needed to clip
    // intersecting faces in the polygon-soup ("ps").

    if (input.verbose) {
        dump_mesh(m0, "m0.v.e"); // containing only vertices & edges
    }

    const uint32_t m0_num_cutpath_edges = (uint32_t)m0_cutpath_edges.size();
    const uint32_t m0_num_cutpath_halfedges = m0_num_cutpath_edges * 2;

    ///////////////////////////////////////////////////////////////////////////
    // Find cut-paths (the boundaries of the openings/holes in the source mesh)
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Find cut-paths ");

    // We are now going to search for all the cut-paths created from the intersection
    // between source- and cut-mesh faces. Some of these cut-paths identify holes to be
    // filled while others indentify separation/slitting of the src-mesh.

    // We start off by creating "bins" : each bin corresonds to an intersection point
    // and the values/elements in that bin are the [cut-path edges] connected to it.

    std::unordered_map<vd_t, std::vector<ed_t>> m0_ivtx_to_cutpath_edges;

    for (std::vector<ed_t>::const_iterator cutpath_edge_iter = m0_cutpath_edges.cbegin();
         cutpath_edge_iter != m0_cutpath_edges.cend();
         ++cutpath_edge_iter) {
        const ed_t& edge = *cutpath_edge_iter;
        const vd_t vertex0 = m0.vertex(edge, 0);
        const vd_t vertex1 = m0.vertex(edge, 1);
        m0_ivtx_to_cutpath_edges[vertex0].push_back(edge);
        m0_ivtx_to_cutpath_edges[vertex1].push_back(edge);
    }

    //... every intersection point is connected to at least onecut-path edge
    MCUT_ASSERT(m0_ivtx_to_cutpath_edges.empty() == false);

    // build implicit cut-path sequences (a sorted set of connected edges)
    // -----------------------------------------------------------------------

    // An "implicit" cut-path sequence is a list of cut-path edges that are sorted (i.e.
    // this means that in memory, edges are placed next to others they connect to).

    std::vector<std::vector<ed_t>> m0_cutpath_sequences;
    std::unordered_map<vd_t, int> m0_ivtx_to_cutpath_sequence;
    std::unordered_map<ed_t, int> m0_edge_to_cutpath_sequence;

    do { // an iteration will build a cut-path sequence

        // const int diff = (int)m0_ivtx_to_cutpath_edges.size() - (int)m0_ivtx_to_cutpath_sequence.size();
        MCUT_ASSERT((int)m0_ivtx_to_cutpath_edges.size() - (int)m0_ivtx_to_cutpath_sequence.size() >= 2); // need a minimum of 2 intersection points (one edge) to form a sequence

        int cur_cutpath_sequence_index = (int)m0_cutpath_sequences.size();

        // start from an intersection point that is not yet mapped-to/associated-with a
        //  sequence in "cur_cutpath_sequence"
        // pick the vertex which is a terminal vertex (to start search from beginning of sequence)
        // or any vertex (if there are not terminal vertices)

        // find any intersection point which is not associated with a cut-path and is connected to one edge (terminal vertex)
        std::unordered_map<vd_t, std::vector<ed_t>>::const_iterator m0_ivtx_to_cutpath_edges_iter = std::find_if(
            m0_ivtx_to_cutpath_edges.cbegin(), m0_ivtx_to_cutpath_edges.cend(),
            [&](const std::pair<vd_t, std::vector<ed_t>>& elem) {
                bool is_mapped = m0_ivtx_to_cutpath_sequence.find(elem.first) != m0_ivtx_to_cutpath_sequence.cend();
                bool is_connected_to_one_edge = elem.second.size() == 1;
                return (!is_mapped && is_connected_to_one_edge);
            });

        if (m0_ivtx_to_cutpath_edges_iter == m0_ivtx_to_cutpath_edges.cend()) { // we could not find any intersection point from above
            // find any intersection point which is not mapped to a cut-path (less strict condition that above)
            m0_ivtx_to_cutpath_edges_iter = std::find_if(
                m0_ivtx_to_cutpath_edges.cbegin(), m0_ivtx_to_cutpath_edges.cend(),
                [&](const std::pair<vd_t, std::vector<ed_t>>& elem) {
                    bool is_mapped = m0_ivtx_to_cutpath_sequence.find(elem.first) != m0_ivtx_to_cutpath_sequence.cend();
                    return !is_mapped;
                });
        }

        if (m0_ivtx_to_cutpath_edges_iter == m0_ivtx_to_cutpath_edges.cend()) { // still could not find any unmapped intersection point
            break; // done (found all implicit cut paths)
        }

        // start new sequence of edges
        // ---------------------------

        m0_cutpath_sequences.emplace_back(std::vector<ed_t>());
        std::vector<ed_t>& cur_cutpath_sequence = m0_cutpath_sequences.back();

        // vertex at the beginning of the sequence
        const vd_t& first_vertex_of_sequence = m0_ivtx_to_cutpath_edges_iter->first;

        // the edges connected to our first intersection point
        const std::vector<ed_t>& cutpath_edges_connected_to_first_vertex = m0_ivtx_to_cutpath_edges_iter->second;

        // pick the edge that is not yet mapped-to/associated-with a  cut-path
        // sequence in "cur_cutpath_sequence". Note: if the current sequence
        // is linear, then there is no possibility that one of the edges
        // in "cutpath_edges_connected_to_first_vertex" has already been mapped-to/associated-with
        // a disjoint cut-path sequence in "cur_cutpath_sequence".
        // This is because sequence discovery starts by first searching from terminal vertices/edges
        // (see above conditions).
        std::vector<ed_t>::const_iterator incident_edge_find_iter = std::find_if(
            cutpath_edges_connected_to_first_vertex.cbegin(),
            cutpath_edges_connected_to_first_vertex.cend(),
            [&](const ed_t& incident_edge) {
                return m0_edge_to_cutpath_sequence.find(incident_edge) == m0_edge_to_cutpath_sequence.cend();
            });

        MCUT_ASSERT(incident_edge_find_iter != cutpath_edges_connected_to_first_vertex.cend());

        const ed_t& first_edge = *incident_edge_find_iter;

        // now we will iteratively add edges into the current sequence, starting from "first_edge".
        // the next added edge is alway one which share's the "next_vertex" with the current.
        // ----------------------------------------------------------------------------------------

        vd_t current_vertex = hmesh_t::null_vertex();
        ed_t current_edge = hmesh_t::null_edge();
        vd_t next_vertex = first_vertex_of_sequence; // ... initial intersection point
        ed_t next_edge = first_edge;

        do { // an iteration will add an edge to the current cut-path sequence

            // update state
            current_vertex = next_vertex;
            current_edge = next_edge;

            // add edge
            cur_cutpath_sequence.emplace_back(current_edge);

            // map vertex to current disjoint implicit cut-path sequence
            MCUT_ASSERT(m0_ivtx_to_cutpath_sequence.count(current_vertex) == 0);
            m0_ivtx_to_cutpath_sequence[current_vertex] = cur_cutpath_sequence_index;
            MCUT_ASSERT(m0_ivtx_to_cutpath_sequence.count(current_vertex) == 1);

            // map edge to current disjoint implicit cut-path sequence
            MCUT_ASSERT(m0_edge_to_cutpath_sequence.count(current_edge) == 0);
            m0_edge_to_cutpath_sequence[current_edge] = cur_cutpath_sequence_index;
            MCUT_ASSERT(m0_edge_to_cutpath_sequence.count(current_edge) == 1);

            // reset state
            next_vertex = hmesh_t::null_vertex();
            next_edge = hmesh_t::null_edge();

            // resolve next vertex (..since we don't know whether vertex0 or vertex1 is "current_vertex")
            const vd_t current_edge_vertex0 = m0.vertex(current_edge, 0);
            const vd_t current_edge_vertex1 = m0.vertex(current_edge, 1);

            // "next_vertex" is whichever vertex of the current edge that is not
            // equal to the "current_vertex"
            if (current_vertex == current_edge_vertex0) {
                next_vertex = current_edge_vertex1;
            } else {
                next_vertex = current_edge_vertex0;
            }

            // now that we have the next vertex, we can determine the next edge
            // ----------------------------------------------------------------

            // check if next vertex has already been associated with the cut-path sequence.
            bool reached_end_of_sequence = m0_ivtx_to_cutpath_sequence.find(next_vertex) != m0_ivtx_to_cutpath_sequence.cend();

            if (!reached_end_of_sequence) {
                // get the other edge connected to "next_vertex" i.e. the edge which is not the "current_edge"
                m0_ivtx_to_cutpath_edges_iter = m0_ivtx_to_cutpath_edges.find(next_vertex);
                MCUT_ASSERT(m0_ivtx_to_cutpath_edges_iter != m0_ivtx_to_cutpath_edges.cend());

                const std::vector<ed_t>& cutpath_edges_connected_to_next_vertex = m0_ivtx_to_cutpath_edges_iter->second;
                MCUT_ASSERT(cutpath_edges_connected_to_next_vertex.size() <= 2);

                bool current_edge_is_terminal = (cutpath_edges_connected_to_next_vertex.size() == 1);

                if (current_edge_is_terminal == false) {
                    const ed_t& edge0 = cutpath_edges_connected_to_next_vertex.front();
                    const ed_t& edge1 = cutpath_edges_connected_to_next_vertex.back();
                    const ed_t& other_edge = (current_edge == edge0) ? edge1 : edge0;

                    // check that "other_edge" has not already been mapped to a disjoint implicit cutpath sequence
                    std::unordered_map<ed_t, int>::const_iterator find_iter = m0_edge_to_cutpath_sequence.find(other_edge);
                    bool other_edge_is_already_mapped = (find_iter != m0_edge_to_cutpath_sequence.cend());

                    if (other_edge_is_already_mapped == false) {
                        next_edge = other_edge; // set sext edge
                    } else {
                        // reached end of sequence
                        MCUT_ASSERT(m0_ivtx_to_cutpath_sequence.count(next_vertex) == 0);
                        // need to update this state here because we wont jump back up to the top of the loop as in the normal case.
                        // This is because "next_edge" is null, and the do-while loop continues iff "next_edge != hmesh_t::null_edge()"
                        m0_ivtx_to_cutpath_sequence[next_vertex] = cur_cutpath_sequence_index;

                        MCUT_ASSERT(m0_ivtx_to_cutpath_sequence.count(next_vertex) == 1);
                    }
                } // if (current_edge_is_terminal == false) {
                else {
                    m0_ivtx_to_cutpath_sequence[next_vertex] = cur_cutpath_sequence_index;
                }
            } // if (!reached_end_of_sequence) {

            // while there is another edge to added to the current disjoint implicit cutpath sequence
        } while (next_edge != hmesh_t::null_edge());

        // while not all intersection-points have been mapped to a disjoint implicit cutpath sequence
    } while (m0_edge_to_cutpath_sequence.size() != m0_cutpath_edges.size());

    MCUT_ASSERT(m0_cutpath_sequences.empty() == false);

    // delink the implicit cut-path sequences to create the final explicit cut-path sequences
    // --------------------------------------------------------------------------------------

    m0_cutpath_edges.clear(); // free

    m0_edge_to_cutpath_sequence.clear(); // free
    // m0_ivtx_to_cutpath_edges.clear();      // free
    // m0_cutpath_sequences.clear(); // free

    TIMESTACK_POP();

    MCUT_ASSERT(m0_cutpath_sequences.empty() == false);

    const int num_explicit_cutpath_sequences = (int)m0_cutpath_sequences.size();

    // save cut-path sequence properties (linear/circular;is_hole)
    // -----------------------------------------------------------------------

    // first we need to find all intersection points which have a border source-mesh
    // halfedge in their intersection registry. We need this data structure to allow
    // us to determine the properties of the cut-paths
    //

    TIMESTACK_PUSH("Infer cutpath info");

    //
    // MapKey=intersection point on a border halfedge of either the source-mesh or cut-mesh
    // MapValue=pointer entry in "m0_ivtx_to_ps_edge"
    std::map<vd_t, std::vector<std::pair<ed_t, fd_t>>::const_iterator> m0_cutpath_terminal_vertices;

    for (std::vector<std::pair<ed_t, fd_t>>::const_iterator iter = m0_ivtx_to_intersection_registry_entry.cbegin();
         iter != m0_ivtx_to_intersection_registry_entry.cend();
         ++iter) {

        const vd_t& ivtx = vd_t((std::uint32_t)std::distance(m0_ivtx_to_intersection_registry_entry.cbegin(), iter) + ps_vtx_cnt); // iter->first;

        const ed_t edge_of_ivtx_ps_he = iter->first; // iter->second.first; // ps.edge(ivtx_ps_he);
        // check that "ivtx_ps_he" is a border halfedge
        if (ps.is_border(edge_of_ivtx_ps_he)) {
            // we have found a terminal vertex
            MCUT_ASSERT(m0_cutpath_terminal_vertices.count(ivtx) == 0);
            m0_cutpath_terminal_vertices[ivtx] = iter;
            MCUT_ASSERT(m0_cutpath_terminal_vertices.count(ivtx) == 1);
        }
    }

    // MapKey=index of an explicit cutpath in  m0_cutpath_sequences
    // MapValue=a tuple of boolean properties (is_linear, is_hole, is_srcmesh_severing).
    //
    // if is_linear is false, then the cut path is "circular" and "is_hole" will
    // always be true in this specific case.
    // if is_linear is true, then the cutpath may or may not be a source-mesh severing cutpath (depends on
    // whether we have a partial cut or not)
    // if is_circular is true, then the cutpath is always severing.
    std::map<int, std::tuple<bool, bool, bool>> m0_cutpath_sequence_to_properties;

    for (std::vector<std::vector<ed_t>>::const_iterator iter = m0_cutpath_sequences.cbegin();
         iter != m0_cutpath_sequences.cend();
         ++iter) {

        const int cutpath_index = (int)std::distance(m0_cutpath_sequences.cbegin(), iter);

        const std::vector<ed_t>& cutpath = *iter;

        MCUT_ASSERT(m0_cutpath_sequence_to_properties.count(cutpath_index) == 0);
        m0_cutpath_sequence_to_properties[cutpath_index] = std::tuple<bool, bool, bool>();

        MCUT_ASSERT(m0_cutpath_sequence_to_properties.count(cutpath_index) == 1);

        std::tuple<bool, bool, bool>& properties = m0_cutpath_sequence_to_properties[cutpath_index];
        bool& cutpath_is_linear = std::get<0>(properties);
        bool& cutpath_is_hole = std::get<1>(properties);
        bool& cutpath_is_srcmesh_severing = std::get<2>(properties); // i.e. the cutpath severs/partitions the src-mesh into two parts

        cutpath_is_linear = false;
        cutpath_is_hole = false;
        cutpath_is_srcmesh_severing = true;

        // check if it is a linear cut path

        const ed_t& first_edge = cutpath.front();
        const vd_t first_edge_vertex0 = m0.vertex(first_edge, 0);
        const vd_t first_edge_vertex1 = m0.vertex(first_edge, 1);
        bool first_edge_vertex0_is_terminal = m0_cutpath_terminal_vertices.find(first_edge_vertex0) != m0_cutpath_terminal_vertices.cend();

        bool first_edge_is_terminal = first_edge_vertex0_is_terminal;

        if (first_edge_vertex0_is_terminal == false) {
            // check if vertex1 is terminal
            bool first_edge_vertex1_is_terminal = m0_cutpath_terminal_vertices.find(first_edge_vertex1) != m0_cutpath_terminal_vertices.cend();
            first_edge_is_terminal = first_edge_vertex1_is_terminal;
        }

        // note: by construction, if the first edge is terminal then the
        // last edge will also be terminal (thus we could have used the
        // last edge for the above tests too!)
        cutpath_is_linear = first_edge_is_terminal;

        bool cutpath_is_circular = !cutpath_is_linear;
        // check if a hole is created by the cutpath (which will need sealing later)
        if (cutpath_is_circular) {
            cutpath_is_hole = true;
        } else {
            // current cut path is [linear]. it creates a hole (in the source mesh) if both terminal vertices
            // have a cut-mesh halfedge in their registry

            const vd_t& first_edge_terminal_vertex = (first_edge_vertex0_is_terminal ? first_edge_vertex0 : first_edge_vertex1);

            // get the halfedge and check where is comes from (cut-mesh/source-mesh)

            std::map<vd_t, std::vector<std::pair<ed_t, fd_t>>::const_iterator>::const_iterator find_iter = m0_cutpath_terminal_vertices.cend();
            find_iter = m0_cutpath_terminal_vertices.find(first_edge_terminal_vertex);

            MCUT_ASSERT(find_iter != m0_cutpath_terminal_vertices.cend());

            // TODO: These variable names are outdated
            const ed_t& first_edge_terminal_vertex_edge = find_iter->second->first;
            const hd_t first_edge_terminal_vertex_edge_h0 = ps.halfedge(first_edge_terminal_vertex_edge, 0);
            fd_t ps_face_of_first_edge_terminal_vertex_he = ps.face(first_edge_terminal_vertex_edge_h0);
            if (ps_face_of_first_edge_terminal_vertex_he == hmesh_t::null_face()) {
                hd_t first_edge_terminal_vertex_edge_h1 = ps.opposite(first_edge_terminal_vertex_edge_h0);
                ps_face_of_first_edge_terminal_vertex_he = ps.face(first_edge_terminal_vertex_edge_h1);
            }

            MCUT_ASSERT(ps_face_of_first_edge_terminal_vertex_he != hmesh_t::null_face());

            bool is_from_cut_mesh = ps_is_cutmesh_face(ps_face_of_first_edge_terminal_vertex_he, sm_face_count);
            bool is_from_src_mesh = !is_from_cut_mesh;

            const bool first_vtx_is_from_src_mesh = is_from_src_mesh;
            /*
        if (is_from_src_mesh) {
            cutpath_is_hole = false;
        }
        else
        {*/
            // ... so the halfedge in the registry of "first_edge_terminal_vertex"
            // belongs to the cut-mesh. Now let us repeat the same test but this
            // time for the "last_edge_terminal_vertex"

            const ed_t& last_edge = cutpath.back();
            const vd_t last_edge_vertex0 = m0.vertex(last_edge, 0);
            const vd_t last_edge_vertex1 = m0.vertex(last_edge, 1);
            bool last_edge_vertex0_is_terminal = m0_cutpath_terminal_vertices.find(last_edge_vertex0) != m0_cutpath_terminal_vertices.cend();

            // bool last_edge_is_terminal = last_edge_vertex0_is_terminal;

            // if (last_edge_vertex0_is_terminal == false)
            //{
            //  check if vertex1 is terminal
            // bool last_edge_vertex1_is_terminal = m0_cutpath_terminal_vertices.find(last_edge_vertex1) != m0_cutpath_terminal_vertices.cend();
            // last_edge_is_terminal = last_edge_vertex1_is_terminal;
            //}

            bool last_vtx_is_from_src_mesh = first_vtx_is_from_src_mesh; // ... we will use this to determine whether we have a severing cutpath or not (the current one)

            // MCUT_ASSERT(last_edge_is_terminal); // i.e. we have a linear cut path
            vd_t last_edge_terminal_vertex = hmesh_t::null_vertex();

            if (last_edge == first_edge) // sequence has one edge
            {
                last_edge_terminal_vertex = (first_edge_terminal_vertex == last_edge_vertex0) ? last_edge_vertex1 : last_edge_vertex0;
            } else {
                last_edge_terminal_vertex = (last_edge_vertex0_is_terminal ? last_edge_vertex0 : last_edge_vertex1);
            }

            // get the halfedge and check where is comes from (cut-mesh/src-mesh)

            // std::map<vd_t, std::map<vd_t, hd_t>::const_iterator>::const_iterator find_iter = m0_cutpath_terminal_vertices.cend();
            find_iter = m0_cutpath_terminal_vertices.find(last_edge_terminal_vertex);
            MCUT_ASSERT(find_iter != m0_cutpath_terminal_vertices.cend());
            const ed_t& last_edge_terminal_vertex_e = find_iter->second->first;
            const hd_t last_edge_terminal_vertex_e_h0 = ps.halfedge(last_edge_terminal_vertex_e, 0);
            fd_t ps_face_of_last_edge_terminal_vertex_he = ps.face(last_edge_terminal_vertex_e_h0);

            if (ps_face_of_last_edge_terminal_vertex_he == hmesh_t::null_face()) {
                const hd_t last_edge_terminal_vertex_e_h1 = ps.opposite(last_edge_terminal_vertex_e_h0);
                ps_face_of_last_edge_terminal_vertex_he = ps.face(last_edge_terminal_vertex_e_h1);
            }

            // must exist because "ivtx_ps_he" came from an intersecting face in the
            // polygon soup
            MCUT_ASSERT(ps_face_of_last_edge_terminal_vertex_he != hmesh_t::null_face());

            is_from_cut_mesh = ps_is_cutmesh_face(ps_face_of_last_edge_terminal_vertex_he, sm_face_count);
            is_from_src_mesh = !is_from_cut_mesh;

            last_vtx_is_from_src_mesh = is_from_src_mesh;

            // if (is_from_cut_mesh) {
            //     cutpath_is_hole = true;
            // }
            //  }
            cutpath_is_hole = (!last_vtx_is_from_src_mesh && !first_vtx_is_from_src_mesh);
            cutpath_is_srcmesh_severing = (first_vtx_is_from_src_mesh && (last_vtx_is_from_src_mesh /* == first_vtx_is_from_src_mesh*/));
            //}
        }
    }

    m0_cutpath_terminal_vertices.clear(); // free

    int num_explicit_linear_cutpaths = 0;
    int num_explicit_circular_cutpaths = 0;
    std::vector<int> explicit_cutpaths_making_holes;
    std::vector<int> explicit_cutpaths_severing_srcmesh;

    for (std::map<int, std::tuple<bool, bool, bool>>::const_iterator iter = m0_cutpath_sequence_to_properties.cbegin();
         iter != m0_cutpath_sequence_to_properties.cend(); ++iter) {
        const int& explicit_cutpath_index = iter->first;
        const std::tuple<bool, bool, bool>& properties = iter->second;

        const bool& is_linear = std::get<0>(properties);
        const bool& is_hole = std::get<1>(properties);
        const bool& is_srcmesh_severing = std::get<2>(properties);

        if (is_linear) {
            num_explicit_linear_cutpaths += 1;
        } else {
            num_explicit_circular_cutpaths += 1;
        }

        if (is_hole) {
            explicit_cutpaths_making_holes.push_back(explicit_cutpath_index);
        }

        if (is_srcmesh_severing) {
            explicit_cutpaths_severing_srcmesh.push_back(explicit_cutpath_index);
        }
    }

    TIMESTACK_POP();

    // NOTE:    at this point we have all vertices, edges, and the lists of
    //          edge sequences identifying the cutpaths
    // =====================================================================

#if 0
        // Detect degeneracy (see note "limitations")
        //--------------------------------------------

        bool have_more_than_one_cutpath = (m0_cutpath_sequences.size() > 0);
        if (have_more_than_one_cutpath)
        {
            bool atleast_one_cutpath_makes_a_hole = !explicit_cutpaths_making_holes.empty();
            //bool atleast_one_explicit_cutpath_is_linear = num_explicit_linear_cutpaths;
            if (atleast_one_cutpath_makes_a_hole)
            {
                // TODO
            }
        }
#endif

    TIMESTACK_PUSH("Detect floating polygons");

    // Detect floating polygons
    // ::::::::::::::::::::::::
    // NOTE: The following code is what we used to determine when to do polygon partitioning in the front end

    // for each circular cut-path (i.e. those making a hole)
    for (std::vector<int>::const_iterator it = explicit_cutpaths_making_holes.cbegin(); it != explicit_cutpaths_making_holes.cend(); ++it) {
        const int cutpath_idx = *it;
        MCUT_ASSERT(m0_cutpath_sequence_to_properties.find(cutpath_idx) != m0_cutpath_sequence_to_properties.cend());
        const std::tuple<bool, bool, bool>& properties = SAFE_ACCESS(m0_cutpath_sequence_to_properties, cutpath_idx);
        const bool& is_linear = std::get<0>(properties);
        bool is_circular = !is_linear;

        if (!is_circular) {
            continue;
        }

        MCUT_ASSERT(cutpath_idx < (int)m0_cutpath_sequences.size());
        const std::vector<ed_t>& cutpath_sequence = SAFE_ACCESS(m0_cutpath_sequences, cutpath_idx);

        MCUT_ASSERT(cutpath_sequence.size() >= 3); // triangle

        // :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        // Detect a floating polygon as "one which is not connectable to other
        // [traced] polygons by an edge". In practice, these polygons are represented
        // by circular cutpaths whose vertices are intersection points that have a
        // tested/intersected face in their registry entry from the same input mesh
        bool is_floating_polygon = true;
        fd_t shared_registry_entry_intersected_face = hmesh_t::null_face();
        vd_t vertex_prev = hmesh_t::null_vertex();
        for (std::vector<ed_t>::const_iterator cp_edge_iter = cutpath_sequence.cbegin(); cp_edge_iter != cutpath_sequence.cend(); cp_edge_iter++) {
            vd_t v = m0.vertex(*cp_edge_iter, 0);
            if (vertex_prev != hmesh_t::null_vertex() && v == vertex_prev) {
                v = m0.vertex(*cp_edge_iter, 1);
                MCUT_ASSERT(v != vertex_prev);
            }

            MCUT_ASSERT(m0_is_intersection_point(v, ps_vtx_cnt));

            // std::map<vd_t, std::pair<ed_t, fd_t>>::const_iterator v_to_intersection_registry_entry = m0_ivtx_to_intersection_registry_entry.find(v);
            if (shared_registry_entry_intersected_face == hmesh_t::null_face()) {
                shared_registry_entry_intersected_face = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)v - ps_vtx_cnt).second; // v_to_intersection_registry_entry->second.second; // set to initial value
            } else {
                // i.e. "is same face" which is being pierced by multiple edges [of the same input mesh]
                is_floating_polygon = (shared_registry_entry_intersected_face == SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)v - ps_vtx_cnt).second /*v_to_intersection_registry_entry->second.second*/);
            }

            if (!is_floating_polygon) {
                break;
            }

            vertex_prev = v;
        }

        vertex_prev = hmesh_t::null_vertex();
        if (is_floating_polygon) {

            // bool ps_face_is_from_cutmesh = ps_is_cutmesh_face(shared_registry_entry_intersected_face, sm_face_count);

            output.detected_floating_polygons[shared_registry_entry_intersected_face].emplace_back(floating_polygon_info_t());
            floating_polygon_info_t& fpi = output.detected_floating_polygons[shared_registry_entry_intersected_face].back();

            // fpi.origin_mesh = ps_face_is_from_cutmesh ? input.cut_mesh : input.src_mesh;
            // const uint32_t cm_faces_start_offset = sm_face_count; // i.e. start offset in "ps"
            // fpi.origin_face = (ps_face_is_from_cutmesh ? fd_t(shared_registry_entry_intersected_face - cm_faces_start_offset) : shared_registry_entry_intersected_face);
            fpi.polygon_vertices.clear();

            std::unordered_map<vd_t, std::vector<ed_t>> ivtx_to_cp_edges;

            for (std::vector<ed_t>::const_iterator cp_edge_iter = cutpath_sequence.cbegin(); cp_edge_iter != cutpath_sequence.cend(); cp_edge_iter++) {
                const vd_t v0 = m0.vertex(*cp_edge_iter, 0);
                const vd_t v1 = m0.vertex(*cp_edge_iter, 1);
                ivtx_to_cp_edges[v0].push_back(*cp_edge_iter);
                ivtx_to_cp_edges[v1].push_back(*cp_edge_iter);
            }

            std::unordered_map<vd_t, std::vector<ed_t>>::const_iterator cur = ivtx_to_cp_edges.cend();
            std::unordered_map<vd_t, std::vector<ed_t>>::const_iterator next = ivtx_to_cp_edges.cbegin();

            ed_t prev_edge = hmesh_t::null_edge();
            do {
                cur = next;
                next = ivtx_to_cp_edges.cend();

                vd_t cur_vertex = cur->first;

                fpi.polygon_vertices.emplace_back(m0.vertex(cur_vertex)); // save coords

                if (prev_edge == hmesh_t::null_edge() || fpi.polygon_vertices.size() < cutpath_sequence.size()) {
                    const std::vector<ed_t>& evec = SAFE_ACCESS(ivtx_to_cp_edges, cur->first);
                    std::vector<ed_t>::const_iterator fiter = std::find_if(evec.cbegin(), evec.cend(), [&](const ed_t& e) { return e != prev_edge; });

                    ed_t edge = *fiter;
                    vd_t next_vertex = m0.vertex(edge, 0);

                    if (next_vertex == cur->first) {
                        next_vertex = m0.vertex(edge, 1);
                    }

                    prev_edge = edge;

                    next = ivtx_to_cp_edges.find(next_vertex);

                    MCUT_ASSERT(next != ivtx_to_cp_edges.cend()); // because we have a loop (floating polygon)
                }

            } while (next != ivtx_to_cp_edges.cend());

            MCUT_ASSERT(ps_tested_face_to_plane_normal.find(shared_registry_entry_intersected_face) != ps_tested_face_to_plane_normal.end());
            fpi.polygon_normal = SAFE_ACCESS(ps_tested_face_to_plane_normal, shared_registry_entry_intersected_face); // used for 2d project

            MCUT_ASSERT(ps_tested_face_to_plane_normal_max_comp.find(shared_registry_entry_intersected_face) != ps_tested_face_to_plane_normal_max_comp.end());
            fpi.polygon_normal_largest_component = SAFE_ACCESS(ps_tested_face_to_plane_normal_max_comp, shared_registry_entry_intersected_face);
        }
    }

    if (!output.detected_floating_polygons.empty()) {
        output.status = status_t::DETECTED_FLOATING_POLYGON;
        return; // abort, so that the front-end can partition ps-faces containing floating polygon
    }

    m0_cutpath_sequence_to_properties.clear();
    m0_ivtx_to_cutpath_edges.clear(); // free

    // The following sections of code are about clipping intersecting polygons,
    // and the information we need in order to do that.
    // -----------------------------------------------------------------------

#if 0
        ///////////////////////////////////////////////////////////////////////////
        // Gather/map intersection points on each intersecting faces
        ///////////////////////////////////////////////////////////////////////////

        

        // TODO: build this data structure during polygon intersection tests!
        std::map<
            fd_t,             // intersectiong face
            std::vector<vd_t> // intersection point which involve the intersecting face
            >
            ps_iface_to_ivtx_list; // faces which intersect with another

        for (std::map<vd_t, std::pair<ed_t, fd_t>>::const_iterator ireg_entry_iter = m0_ivtx_to_intersection_registry_entry.cbegin();
             ireg_entry_iter != m0_ivtx_to_intersection_registry_entry.cend();
             ++ireg_entry_iter)
        { // for each intersection point ...

            const vd_t &intersection_point = ireg_entry_iter->first;
            const std::vector<fd_t> entry_faces = ps_get_ivtx_registry_entry_faces(ps, ireg_entry_iter->second); // faces in registry entry

            // update face vertex-registry

            for (std::vector<fd_t>::const_iterator entry_face_iter = entry_faces.cbegin();
                 entry_face_iter != entry_faces.cend();
                 ++entry_face_iter)
            { // for each face in the intersection point's registry entry

                if (is_virtual_face(*entry_face_iter))
                {
                    continue; // virtual faces are simply placeholders - not useful
                }

                // get entry of current face
                std::map<fd_t, std::vector<vd_t>>::iterator find_iter = ps_iface_to_ivtx_list.find(*entry_face_iter);
                const bool face_vertex_registery_exists = find_iter != ps_iface_to_ivtx_list.cend();

                if (face_vertex_registery_exists)
                { // do we have a map entry already...?
                    // get the intersection points incident to the face
                    std::vector<vd_t> &face_vertex_registry = find_iter->second;
                    // has the current intersection point been associated with the current intersecting face
                    const bool vertex_registered = std::find(face_vertex_registry.cbegin(), face_vertex_registry.cend(), intersection_point) != face_vertex_registry.cend();

                    if (!vertex_registered)
                    {
                        face_vertex_registry.push_back(intersection_point); // associate intersection point with intersecting face
                    }
                }
                else
                {
                    // add face-registry and register vertex
                    std::pair<std::map<fd_t, std::vector<vd_t>>::iterator, bool> pair = ps_iface_to_ivtx_list.insert(std::make_pair(*entry_face_iter, std::vector<vd_t>()));
                    MCUT_ASSERT(pair.second == true);

                    find_iter = pair.first;
                    std::vector<vd_t> &face_vertex_registry = find_iter->second;
                    face_vertex_registry.push_back(intersection_point); // associate intersection point with intersecting face
                }
            }
        }

        for (std::map<fd_t, std::vector<vd_t>>::const_iterator i = ps_iface_to_ivtx_list.cbegin(); i != ps_iface_to_ivtx_list.cend(); ++i)
        {
            
            

            // log
            
            for (std::vector<vd_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j)
            {
                
            }
            
            

            
        }
#endif

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // Create new edges partitioning the intersecting ps edges (2-part process)
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Create polygon-exterior edges (w/ > 3 vertices)");

    // Part 1
    //
    // Here, we identify ps-edges with more than 3 coincident m0-vertices (ps-
    // and intersection points)
    //
    // Task: 1) find ps-edges with more than 3 coincident m0-vertices, 2)
    // sort these vertices along the ps-edge 3) connect sorted point by
    // creating edges in "m0"
    //
    // Brief: ps-edges with more than 3 coincident vertices arise during a
    // partial-cut (3d polyhedron) and/or concave cut-mesh-to-source-mesh
    // face intersection.
    // For every such edge, there will be 2 ps-vertices and the rest are
    // intersection points. (Sorting requires numerical calculation).
    //
    // We also create a mapping between each polygon-boundary interior-edge
    // vertex and its multiple copies which will be used for connected component
    // separation and sealing (hole filling).
    // NOTE: a polygon-boundary edge is one which is lies on the boundary of a
    // ps-polygon. Conversely, an interior edge lies within the polygon (path
    // along which polygon is clipped - the cut path).

    // std::map<vd_t, std::vector<vd_t>> m0_to_m1_poly_ext_int_edge_vertex;

    std::unordered_map<ed_t, std::vector<std::pair<vd_t, vec3>>> ps_edge_to_vertices; // stores ps-edges with more-than 3 coincident vertices

    for (std::unordered_map<ed_t, std::vector<vd_t>>::const_iterator iter_ps_edge = ps_intersecting_edges.cbegin(); iter_ps_edge != ps_intersecting_edges.cend(); ++iter_ps_edge) {

        // TODO: get_vertices_on_ps_edge() is not needed we can probably infer this information using previously/pre-computed std::maps
        // vertices that lie on current ps edge

        // get_vertices_on_ps_edge(iter_ps_edge, m0_ivtx_to_intersection_registry_entry, ps, m0_to_ps_vtx);

        if (iter_ps_edge->second.size() > 1) { // intersection points on edge is more than 1 i.e. edge has more than three vertices

            const vd_t ps_v0 = ps.vertex(iter_ps_edge->first, 0);
            const vd_t ps_v1 = ps.vertex(iter_ps_edge->first, 1);

            MCUT_ASSERT((int)(ps_v0) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_v0) != ps_to_m0_vtx.cend())*/);
            const vd_t m0_v0 = ps_to_m0_vtx[ps_v0];
            MCUT_ASSERT((int)(ps_v1) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_v1) != ps_to_m0_vtx.cend()*/);
            const vd_t m0_v1 = ps_to_m0_vtx[ps_v1];
            std::vector<vd_t> vertices_on_ps_edge = { m0_v0, m0_v1 };

            // and rest of points (intersection points)
            vertices_on_ps_edge.insert(vertices_on_ps_edge.end(), iter_ps_edge->second.cbegin(), iter_ps_edge->second.cend());

            MCUT_ASSERT(ps_edge_to_vertices.find(iter_ps_edge->first) == ps_edge_to_vertices.end()); // edge cannot have been traversed before!

            ps_edge_to_vertices.insert(std::make_pair(iter_ps_edge->first, std::vector<std::pair<vd_t, vec3>>()));

            for (std::vector<vd_t>::const_iterator it = vertices_on_ps_edge.cbegin(); it != vertices_on_ps_edge.cend(); ++it) {

                const vec3& vertex_coordinates = m0.vertex(*it); // get the coordinates (for sorting)
                SAFE_ACCESS(ps_edge_to_vertices, iter_ps_edge->first).push_back(std::make_pair(*it, vertex_coordinates));

                // if (m0_is_intersection_point(*it, ps_vtx_cnt)) { // is intersection point
                // m0_to_m1_poly_ext_int_edge_vertex.insert(std::make_pair(*it, std::vector<vd_t>()));
                // }
            }
        }
    }

    // In the next for-loop, we sort each list of vertices on each ps-edge
    // which more than 3 coincident vertices

    std::unordered_map<ed_t, std::vector<vd_t>> ps_edge_to_sorted_descriptors; // sorted vertex that lie on each edge with > 3 vertices

    for (std::unordered_map<ed_t, std::vector<std::pair<vd_t, vec3>>>::iterator edge_vertices_iter = ps_edge_to_vertices.begin(); edge_vertices_iter != ps_edge_to_vertices.end(); ++edge_vertices_iter) {
        std::vector<std::pair<vd_t, vec3>>& incident_vertices = edge_vertices_iter->second;

        ps_edge_to_sorted_descriptors[edge_vertices_iter->first] = linear_projection_sort(incident_vertices);

#if 0
      // since all points are on straight line, we sort them by x-coord and by y-coord if x-coord is the same for all vertices
      std::sort(incident_vertices.begin(), incident_vertices.end(),
        [&](const std::pair<vd_t, vec3>& a, const std::pair<vd_t, vec3>& b) {
          return (a.second.x() < b.second.x());
        });

      const bool x_coordinate_is_same = have_same_coordinate(incident_vertices, 0);

      if (x_coordinate_is_same) {
        // ... then  sort on y-coord
        std::sort(incident_vertices.begin(), incident_vertices.end(),
          [&](const std::pair<vd_t, vec3>& a, const std::pair<vd_t, vec3>& b) {
            return (a.second.y() < b.second.y());
          });

        const bool y_coordinate_is_same = have_same_coordinate(incident_vertices, 1);

        if (y_coordinate_is_same) {
          // ... then  sort on z-coord
          std::sort(incident_vertices.begin(), incident_vertices.end(),
            [&](const std::pair<vd_t, vec3>& a, const std::pair<vd_t, vec3>& b) {
              return (a.second.z() < b.second.z());
            });
        }
      }

#endif
    }

    //
    // Now we, create edges between the sorted vertices that are coincident
    // on the same ps-edge that has more-than 3 incident vertices.
    //
    // This step will create class-1 (o==>x), class-2 (o==>x),
    // and class-3 (x==>x) which are the so called "polygon-boundary
    // interior-iedges".

    std::map<
        ed_t, // polygon-soup edge
        std::vector<ed_t> // list of m0-edges which lay on polygon-soup edge
        >
        ps_to_m0_edges;

    // for each ps-edge with more than 3 coincindent vertices
    for (std::unordered_map<ed_t, std::vector<vd_t>>::const_iterator ps_edge_coincident_vertices_iter = ps_edge_to_sorted_descriptors.begin();
         ps_edge_coincident_vertices_iter != ps_edge_to_sorted_descriptors.end();
         ++ps_edge_coincident_vertices_iter) {

        // get sorted list of vertices on edge
        const std::vector<vd_t>& coincident_sorted_vertices = ps_edge_coincident_vertices_iter->second;

        MCUT_ASSERT(coincident_sorted_vertices.size() > 3); // we are only dealing with ps-edges with more than 3 coicindent vertices

        // first vertex must not be an intersection point, because all vertices lie on a
        // ps-edge to be partitioned into new edges, thus the first vertex must not
        // be an intersection point: [*]===========[*] --> [*]===*==*======[*]
        MCUT_ASSERT(!m0_is_intersection_point(coincident_sorted_vertices.front(), ps_vtx_cnt));
        MCUT_ASSERT(m0_is_intersection_point((*(coincident_sorted_vertices.cbegin() + 1)), ps_vtx_cnt));

        MCUT_ASSERT(m0_is_intersection_point((*(coincident_sorted_vertices.cend() - 2)), ps_vtx_cnt));
        MCUT_ASSERT(!m0_is_intersection_point(coincident_sorted_vertices.back(), ps_vtx_cnt)); // likewise, last vertex must not be an intersection point

        // for each sorted vertex on ps-edge (starting from the second in the list)
        for (std::vector<vd_t>::const_iterator iter = coincident_sorted_vertices.cbegin() + 1; iter != coincident_sorted_vertices.cend(); ++iter) {

            const vd_t src_vertex = *(iter - 1);
            const vd_t tgt_vertex = *(iter);
            const hd_t h = m0.add_edge(src_vertex, tgt_vertex); // create edge!

            MCUT_ASSERT(h != hmesh_t::null_halfedge());

            const ed_t new_edge = m0.edge(h);
            MCUT_ASSERT(new_edge != hmesh_t::null_edge());

            // map original ps-edge to list of "child" edges which lie on it
            ps_to_m0_edges[ps_edge_coincident_vertices_iter->first].push_back(new_edge);

            // Here we save the "incoming" halfedge for each vertex of the created edge,
            // if the vertex is an intersection point. An incoming halfedge is a one
            // whose target is the vertex.
            // We will using this information when splitting the source mesh along the
            // cut path (when duplicating intersection points to create holes).

            if ((iter - 1) == coincident_sorted_vertices.cbegin()) // first iteration
            {
                MCUT_ASSERT(m0.target(h) == tgt_vertex);
                ivtx_to_incoming_hlist[tgt_vertex].push_back(h);
            } else if ((std::size_t)std::distance(coincident_sorted_vertices.cbegin(), iter) == coincident_sorted_vertices.size() - 1) // last iterator
            {
                MCUT_ASSERT(m0.target(m0.opposite(h)) == src_vertex);
                ivtx_to_incoming_hlist[src_vertex].push_back(m0.opposite(h));
            } else {
                MCUT_ASSERT(m0.target(h) == tgt_vertex);
                ivtx_to_incoming_hlist[tgt_vertex].push_back(h);

                MCUT_ASSERT(m0.target(m0.opposite(h)) == src_vertex);
                ivtx_to_incoming_hlist[src_vertex].push_back(m0.opposite(h));
            }

            // Here, we also associate the new edge with an intersecting ps-face.
            // Note: since the new edge here will lie on the face boundary, its associated intersecting ps-face(s) will
            // be those which are incident to the parent ps-edge
            const ed_t ps_edge = ps_edge_coincident_vertices_iter->first;

            for (int i = 0; i < 2; ++i) { // for each halfedge of edge
                const hd_t ps_edge_h = ps.halfedge(ps_edge, i);

                if (ps_edge_h != hmesh_t::null_halfedge()) {
                    const fd_t f = ps.face(ps_edge_h);
                    if (f != hmesh_t::null_face()) // ps_edge could be on the border!
                    {
                        ps_iface_to_m0_edge_list[f].emplace_back(new_edge);
                    }
                }
            }
        }
    }

    TIMESTACK_POP();

    TIMESTACK_PUSH("Create polygon-exterior edges (2 or 3 vertices)");

    // Part 2
    //
    // We will now create edges between vertices that lie on the same ps-edge
    // which has 2 or 3 coincident vertices. Note that in the case of 2 coincident
    // vertices, the created edge is the same as the original ps-edge.
    //
    // Brief: the order of m0-vertices along the ps-edge is deduced since the number
    // of vertices is small enough (unlike Part 1).
    // So we have two simple cases:
    // a) ps-edge is coincident on two m0-vertices which are not intersection points
    // b) ps-edge is coincident on three m0-vertices such that one is an intersection point
    //

    // a map between edge ids in "ps" and in "m0", which is the data structure we are progressively
    // defining to hold data for the new mesh containing clipped polygons
    std::unordered_map<ed_t, ed_t> ps_to_m0_non_intersecting_edge;

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef edge_array_iterator_t InputStorageIteratorType;
        typedef std::tuple<
            std::unordered_map<ed_t, ed_t>, // ps_to_m0_non_intersecting_edge
            std::unordered_map<fd_t, std::vector<ed_t>>, // ps_iface_to_m0_edge_list
            std::unordered_map<vd_t, std::vector<hd_t>>, // ivtx_to_incoming_hlist
            // locally computed edge
            // local-edge-id to the m0-vertex-descriptors that are used to create edge
            std::vector<std::pair<vd_t, vd_t>>>
            OutputStorageTypesTuple;

        auto fn_compute_polygon_boundary_edges = [&](
                                                     InputStorageIteratorType block_start_,
                                                     InputStorageIteratorType block_end_) -> OutputStorageTypesTuple {
            OutputStorageTypesTuple local_output;

            std::unordered_map<ed_t, ed_t>& ps_to_m0_non_intersecting_edge_LOCAL = std::get<0>(local_output);
            std::unordered_map<fd_t, std::vector<ed_t>>& ps_iface_to_m0_edge_list_LOCAL = std::get<1>(local_output);
            std::unordered_map<vd_t, std::vector<hd_t>>& ivtx_to_incoming_hlist_LOCAL = std::get<2>(local_output);
            std::vector<std::pair<vd_t, vd_t>>& edges_LOCAL = std::get<3>(local_output);

            const uint32_t rough_number_of_edges = (uint32_t)std::distance(block_start_, block_end_);
            edges_LOCAL.reserve((uint32_t)(rough_number_of_edges * 1.2)); // most edges are original

            for (edge_array_iterator_t iter_ps_edge = block_start_; iter_ps_edge != block_end_; ++iter_ps_edge) {
                // std::cout << (uint32_t)(*iter_ps_edge) << std::endl;
                if (ps_edge_to_vertices.find(*iter_ps_edge) != ps_edge_to_vertices.end()) {
                    continue; // the case of more than 3 vertices (handled above)
                }

                const ed_t ps_edge = *iter_ps_edge; // edge handle
                const vd_t ps_v0 = ps.vertex(ps_edge, 0);
                const vd_t ps_v1 = ps.vertex(ps_edge, 1);

                MCUT_ASSERT((int)(ps_v0) < (int)ps_to_m0_vtx.size());
                const vd_t m0_v0 = ps_to_m0_vtx[ps_v0];
                MCUT_ASSERT((int)(ps_v1) < (int)ps_to_m0_vtx.size());
                const vd_t m0_v1 = ps_to_m0_vtx[ps_v1];

                std::vector<vd_t> vertices_on_ps_edge = { ps_v0, ps_v1 };
                std::unordered_map<ed_t, std::vector<vd_t>>::const_iterator ps_intersecting_edges_iter = ps_intersecting_edges.find(ps_edge);
                if (ps_intersecting_edges_iter != ps_intersecting_edges.cend()) {
                    vertices_on_ps_edge.insert(
                        vertices_on_ps_edge.end(),
                        ps_intersecting_edges_iter->second.cbegin(),
                        ps_intersecting_edges_iter->second.cend());
                }

                if (vertices_on_ps_edge.size() == 2) {
                    // const hd_t h = m0.add_edge(vertices_on_ps_edge.back(), vertices_on_ps_edge.front());
                    // MCUT_ASSERT(h != hmesh_t::null_halfedge());
                    edges_LOCAL.push_back(std::make_pair(vertices_on_ps_edge.back(), vertices_on_ps_edge.front()));
                    const ed_t edge = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                    const hd_t h(edge * 2);

                    ps_to_m0_non_intersecting_edge_LOCAL[ps_edge] = edge;

                    for (int i = 0; i < 2; ++i) {
                        // const hd_t ps_edge_h = ps.halfedge(ps_edge, i);
                        const hd_t ps_edge_h = ps.halfedge(ps_edge, i);
                        if (ps_edge_h != hmesh_t::null_halfedge()) {
                            const fd_t f = ps.face(ps_edge_h);
                            // NOTE: ps_iface_to_m0_edge_list already contains [all] intersecting ps faces,
                            // which where added when we compute edge between intersection points.
                            // We associate "f" with "edge" using the local variable "ps_iface_to_m0_edge_list_LOCAL"
                            // because "ps_iface_to_m0_edge_list" is shared by all threads. Thus, each thread computes
                            // its local output first, and then we'll merge it into "ps_iface_to_m0_edge_list" later
                            bool is_intersecting_ps_face = f != hmesh_t::null_face() && ps_iface_to_m0_edge_list.find(f) != ps_iface_to_m0_edge_list.cend();
                            if (is_intersecting_ps_face) {
                                ps_iface_to_m0_edge_list_LOCAL[f].emplace_back(edge);
                            }
                        }
                    }
                } else {

                    MCUT_ASSERT(vertices_on_ps_edge.size() == 3);

                    const vd_t first = vertices_on_ps_edge[0];
                    const vd_t second = vertices_on_ps_edge[1];
                    const vd_t third = vertices_on_ps_edge[2];

                    hd_t h0;
                    hd_t h1;

                    ed_t m0_h0_edge_local;
                    ed_t m0_h1_edge_local;

                    if (!m0_is_intersection_point(first, ps_vtx_cnt)) { // o-->...
                        if (m0_is_intersection_point(second, ps_vtx_cnt)) {

                            // h0 = m0.add_edge(first, second);
                            // MCUT_ASSERT(h0 != hmesh_t::null_halfedge());
                            edges_LOCAL.push_back(std::make_pair(first, second));
                            m0_h0_edge_local = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                            h0 = hd_t(m0_h0_edge_local * 2); // mimmick halfedge descriptor

                            // MCUT_ASSERT(m0.target(h0) == second);
                            ivtx_to_incoming_hlist_LOCAL[second].push_back(h0);

                            // h1 = m0.add_edge(second, third);
                            // MCUT_ASSERT(h1 != hmesh_t::null_halfedge());

                            edges_LOCAL.push_back(std::make_pair(second, third));
                            m0_h1_edge_local = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                            h1 = hd_t(m0_h1_edge_local * 2);

                            // MCUT_ASSERT(m0.target(m0.opposite(h1)) == second);
                            // ivtx_to_incoming_hlist_LOCAL[second].push_back(m0.opposite(h1));
                            hd_t h1_opp_local = hd_t((uint32_t)h1 + 1);
                            ivtx_to_incoming_hlist_LOCAL[second].push_back(h1_opp_local);
                        } else {
                            // h0 = m0.add_edge(first, third);
                            // MCUT_ASSERT(h0 != hmesh_t::null_halfedge());
                            edges_LOCAL.push_back(std::make_pair(first, third));
                            m0_h0_edge_local = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                            h0 = hd_t(m0_h0_edge_local * 2);

                            ivtx_to_incoming_hlist_LOCAL[third].push_back(h0);

                            // h1 = m0.add_edge(third, second);
                            // MCUT_ASSERT(h1 != hmesh_t::null_halfedge());
                            edges_LOCAL.push_back(std::make_pair(third, second));
                            m0_h1_edge_local = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                            h1 = hd_t(m0_h1_edge_local * 2);

                            // ivtx_to_incoming_hlist_LOCAL[third].push_back(m0.opposite(h1));
                            hd_t h1_opp_local = hd_t((uint32_t)h1 + 1);
                            ivtx_to_incoming_hlist_LOCAL[third].push_back(h1_opp_local);
                        }
                    } else {
                        // h0 = m0.add_edge(second, first); // o-->x
                        // MCUT_ASSERT(h0 != hmesh_t::null_halfedge());
                        edges_LOCAL.push_back(std::make_pair(second, first));
                        m0_h0_edge_local = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                        h0 = hd_t(m0_h0_edge_local * 2);

                        ivtx_to_incoming_hlist_LOCAL[first].push_back(h0);

                        // MCUT_ASSERT(m0.target(m0.opposite(h0)) == second);
                        // h1 = m0.add_edge(first, third);
                        // MCUT_ASSERT(h1 != hmesh_t::null_halfedge());
                        edges_LOCAL.push_back(std::make_pair(first, third));
                        m0_h1_edge_local = ed_t((ed_t::index_type)(edges_LOCAL.size() - 1));
                        h1 = hd_t(m0_h1_edge_local * 2);

                        // MCUT_ASSERT(m0.target(m0.opposite(h1)) == first);
                        // ivtx_to_incoming_hlist_LOCAL[first].push_back(m0.opposite(h1));
                        hd_t h1_opp_local = hd_t((uint32_t)h1 + 1);
                        ivtx_to_incoming_hlist_LOCAL[first].push_back(h1_opp_local);
                    }

                    for (int i = 0; i < 2; ++i) {
                        const hd_t ps_edge_h = ps.halfedge(ps_edge, i);
                        if (ps_edge_h != hmesh_t::null_halfedge()) {
                            const fd_t f = ps.face(ps_edge_h);
                            if (f != hmesh_t::null_face()) {
                                // ps_iface_to_m0_edge_list_LOCAL[f].emplace_back(m0.edge(h0));
                                ps_iface_to_m0_edge_list_LOCAL[f].emplace_back(m0_h0_edge_local);
                                // ps_iface_to_m0_edge_list_LOCAL[f].emplace_back(m0.edge(h1));
                                ps_iface_to_m0_edge_list_LOCAL[f].emplace_back(m0_h1_edge_local);
                            }
                        }
                    }
                }
            }

            return local_output;
        };

        std::vector<std::future<OutputStorageTypesTuple>> futures;
        OutputStorageTypesTuple partial_res;

        parallel_for(
            *input.scheduler,
            ps.edges_begin(),
            ps.edges_end(),
            fn_compute_polygon_boundary_edges,
            partial_res, // output computed by master thread
            futures);

        const std::unordered_map<ed_t, ed_t> ps_to_m0_non_intersecting_edge_MASTER_THREAD_LOCAL = std::get<0>(partial_res);
        const std::unordered_map<fd_t, std::vector<ed_t>>& ps_iface_to_m0_edge_list_MASTER_THREAD_LOCAL = std::get<1>(partial_res);
        const std::unordered_map<vd_t, std::vector<hd_t>>& ivtx_to_incoming_hlist_MASTER_THREAD_LOCAL = std::get<2>(partial_res);
        // local-edge-id to the m0-vertex-descriptors that are used to create edge
        // the sequence of elements signifies the order in which they were computed
        const std::vector<std::pair<vd_t, vd_t>>& edge_create_info_MASTER_THREAD_LOCAL = std::get<3>(partial_res);

        // NOTE: this lambda adds the edges stored in "edge_create_info_" into m0
        // the other iterable parameters are simply updated (i.e. ed_t of "m0" variables are given their proper offset)
        auto merge_local_m0_edges = [](
                                        hmesh_t& m0_,
                                        const std::unordered_map<ed_t, ed_t>& ps_to_m0_non_intersecting_edge_FUTURE,
                                        const std::unordered_map<fd_t, std::vector<ed_t>>& ps_iface_to_m0_edge_list_FUTURE,
                                        const std::unordered_map<vd_t, std::vector<hd_t>>& ivtx_to_incoming_hlist_FUTURE,
                                        const std::vector<std::pair<vd_t, vd_t>>& edge_create_info_FUTURE,
                                        std::unordered_map<ed_t, ed_t>& ps_to_m0_non_intersecting_edge,
                                        std::unordered_map<fd_t, std::vector<ed_t>>& ps_iface_to_m0_edge_list,
                                        std::unordered_map<vd_t, std::vector<hd_t>>& ivtx_to_incoming_hlist) {
            std::vector<ed_t> emap(edge_create_info_FUTURE.size());
            // add edges using edge_create_info
            for (std::vector<std::pair<vd_t, vd_t>>::const_iterator it = edge_create_info_FUTURE.begin();
                 it != edge_create_info_FUTURE.end();
                 ++it) {
                hd_t h = m0_.add_edge(it->first, it->second);
                MCUT_ASSERT(h % 2 == 0); // this is what allows indexing trick
                // MCUT_ASSERT(ed_t((uint32_t)h/2) == ed_t(m0_edge_descr_baseoffset_ + it->first));
                size_t local_edge_idx = std::distance(edge_create_info_FUTURE.cbegin(), it);
                emap[local_edge_idx] = m0_.edge(h);
            }

            // merge ps_to_m0_non_intersecting_edge

            for (std::unordered_map<ed_t, ed_t>::const_iterator it = ps_to_m0_non_intersecting_edge_FUTURE.cbegin();
                 it != ps_to_m0_non_intersecting_edge_FUTURE.cend();
                 ++it) {
                ps_to_m0_non_intersecting_edge[it->first] = SAFE_ACCESS(emap, it->second);
            }

            // merge ps_iface_to_m0_edge_list
            for (std::unordered_map<fd_t, std::vector<ed_t>>::const_iterator it = ps_iface_to_m0_edge_list_FUTURE.cbegin();
                 it != ps_iface_to_m0_edge_list_FUTURE.cend();
                 ++it) {
                for (std::vector<ed_t>::const_iterator i = it->second.cbegin(); i != it->second.cend(); ++i) {
                    ps_iface_to_m0_edge_list[it->first].push_back(SAFE_ACCESS(emap, *i));
                }
            }

            // merge ivtx_to_incoming_hlist
            for (std::unordered_map<vd_t, std::vector<hd_t>>::const_iterator it = ivtx_to_incoming_hlist_FUTURE.cbegin();
                 it != ivtx_to_incoming_hlist_FUTURE.cend();
                 ++it) {
                for (std::vector<hd_t>::const_iterator i = it->second.cbegin(); i != it->second.cend(); ++i) {
                    hd_t he_local = *i;
                    ed_t he_edge_local = ed_t((uint32_t)he_local / 2);
                    ivtx_to_incoming_hlist[it->first].push_back(m0_.halfedge(SAFE_ACCESS(emap, he_edge_local), (uint32_t)(he_local % 2)));
                }
            }
        };

        // add edges computed by master thread and update local edge (and halfedge) descriptors
        merge_local_m0_edges(
            m0,
            ps_to_m0_non_intersecting_edge_MASTER_THREAD_LOCAL,
            ps_iface_to_m0_edge_list_MASTER_THREAD_LOCAL,
            ivtx_to_incoming_hlist_MASTER_THREAD_LOCAL,
            edge_create_info_MASTER_THREAD_LOCAL,
            ps_to_m0_non_intersecting_edge,
            ps_iface_to_m0_edge_list,
            ivtx_to_incoming_hlist);

        // merge thread-local output into global data structures
        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<OutputStorageTypesTuple>& f = futures[i];
            MCUT_ASSERT(f.valid());
            OutputStorageTypesTuple future_result = f.get(); // "get()" is a blocking function

            const std::unordered_map<ed_t, ed_t>& ps_to_m0_non_intersecting_edge_FUTURE = std::get<0>(future_result);
            const std::unordered_map<fd_t, std::vector<ed_t>>& ps_iface_to_m0_edge_list_FUTURE = std::get<1>(future_result);
            const std::unordered_map<vd_t, std::vector<hd_t>>& ivtx_to_incoming_hlist_FUTURE = std::get<2>(future_result);
            const std::vector<std::pair<vd_t, vd_t>>& edge_create_info_FUTURE = std::get<3>(future_result);

            merge_local_m0_edges(
                m0,
                ps_to_m0_non_intersecting_edge_FUTURE,
                ps_iface_to_m0_edge_list_FUTURE,
                ivtx_to_incoming_hlist_FUTURE,
                edge_create_info_FUTURE,
                ps_to_m0_non_intersecting_edge,
                ps_iface_to_m0_edge_list,
                ivtx_to_incoming_hlist);
        }

    } // end of parallel scope

#else

    // for each ps-edge
    for (edge_array_iterator_t iter_ps_edge = ps.edges_begin(); iter_ps_edge != ps.edges_end(); ++iter_ps_edge) {

        if (ps_edge_to_vertices.empty() == false && ps_edge_to_vertices.find(*iter_ps_edge) != ps_edge_to_vertices.end()) {
            continue; // the case of more than 3 vertices (handled above)
        }

        const ed_t ps_edge = *iter_ps_edge; // edge handle
        const vd_t ps_v0 = ps.vertex(ps_edge, 0);
        const vd_t ps_v1 = ps.vertex(ps_edge, 1);

        MCUT_ASSERT((int)(ps_v0) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_v0) != ps_to_m0_vtx.cend())*/);
        const vd_t m0_v0 = ps_to_m0_vtx[ps_v0];
        MCUT_ASSERT((int)(ps_v1) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_v1) != ps_to_m0_vtx.cend()*/);
        const vd_t m0_v1 = ps_to_m0_vtx[ps_v1];

        std::vector<vd_t> vertices_on_ps_edge = { ps_v0, ps_v1 }; // get_vertices_on_ps_edge(*iter_ps_edge, m0_ivtx_to_ps_edge, ps, m0_to_ps_vtx);
        std::unordered_map<ed_t, std::vector<vd_t>>::const_iterator ps_intersecting_edges_iter = ps_intersecting_edges.find(ps_edge);
        if (ps_intersecting_edges_iter != ps_intersecting_edges.cend()) {
            vertices_on_ps_edge.insert(vertices_on_ps_edge.end(), ps_intersecting_edges_iter->second.cbegin(), ps_intersecting_edges_iter->second.cend());
        }

        const uint32_t num_vertices_on_ps_edge = vertices_on_ps_edge.size();

        if (num_vertices_on_ps_edge == 2u) // simple case (edge did not intersect with any polygon)
        {

            const hd_t h = m0.add_edge(vertices_on_ps_edge[1], vertices_on_ps_edge[0]);

            MCUT_ASSERT(h != hmesh_t::null_halfedge());

            const ed_t edge = m0.edge(h);
            ps_to_m0_non_intersecting_edge[ps_edge] = edge; // associate

            // similar to Part 1, we also associate the new edge with an intersecting ps-face.
            for (int i = 0; i < 2; ++i) {
                const hd_t ps_edge_h = ps.halfedge(ps_edge, i);
                if (ps_edge_h != hmesh_t::null_halfedge()) { // note: ps_edge could be on the border!
                    const fd_t f = ps.face(ps_edge_h);
                    bool is_intersecting_ps_face = f != hmesh_t::null_face() && ps_iface_to_m0_edge_list.find(f) != ps_iface_to_m0_edge_list.cend();
                    if (is_intersecting_ps_face) {
                        ps_iface_to_m0_edge_list[f].emplace_back(edge);
                    }
                }
            }
        } else { // this is the more complex case where we add minimal set of non overlapping edges between 3 vertices

            MCUT_ASSERT(num_vertices_on_ps_edge == 3u);

            const vd_t first = vertices_on_ps_edge[0];
            const vd_t second = vertices_on_ps_edge[1];
            const vd_t third = vertices_on_ps_edge[2];

            hd_t h0;
            hd_t h1;

            if (!m0_is_intersection_point(first, ps_vtx_cnt)) { // o-->...
                if (m0_is_intersection_point(second, ps_vtx_cnt)) {
                    //
                    // o x o
                    //

                    h0 = m0.add_edge(first, second);
                    MCUT_ASSERT(h0 != hmesh_t::null_halfedge());

                    MCUT_ASSERT(m0.target(h0) == second);
                    ivtx_to_incoming_hlist[second].push_back(h0);

                    h1 = m0.add_edge(second, third);
                    MCUT_ASSERT(h1 != hmesh_t::null_halfedge());

                    MCUT_ASSERT(m0.target(m0.opposite(h1)) == second);
                    ivtx_to_incoming_hlist[second].push_back(m0.opposite(h1));
                } else {
                    //
                    //  o o x
                    //

                    h0 = m0.add_edge(first, third);
                    MCUT_ASSERT(h0 != hmesh_t::null_halfedge());
                    ivtx_to_incoming_hlist[third].push_back(h0);

                    h1 = m0.add_edge(third, second);
                    MCUT_ASSERT(h1 != hmesh_t::null_halfedge());
                    ivtx_to_incoming_hlist[third].push_back(m0.opposite(h1));
                }
            } else {
                //
                // x o o
                //

                h0 = m0.add_edge(second, first); // o-->x
                MCUT_ASSERT(h0 != hmesh_t::null_halfedge());
                ivtx_to_incoming_hlist[first].push_back(h0);

                MCUT_ASSERT(m0.target(m0.opposite(h0)) == second);

                h1 = m0.add_edge(first, third); // x-->o
                MCUT_ASSERT(h1 != hmesh_t::null_halfedge());

                MCUT_ASSERT(m0.target(m0.opposite(h1)) == first);
                ivtx_to_incoming_hlist[first].push_back(m0.opposite(h1));
            }

            // // associate the new edge with an intersecting ps-face
            for (int i = 0; i < 2; ++i) { // for each halfedge of edge
                const hd_t ps_edge_h = ps.halfedge(ps_edge, i);

                if (ps_edge_h != hmesh_t::null_halfedge()) {
                    const fd_t f = ps.face(ps_edge_h);
                    if (f != hmesh_t::null_face()) // ps_edge could be on the border!
                    {
                        ps_iface_to_m0_edge_list[f].emplace_back(m0.edge(h0));
                        ps_iface_to_m0_edge_list[f].emplace_back(m0.edge(h1));
                    }
                }
            }
        }
    }

#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

    TIMESTACK_POP(); // &&&&&

    ps_intersecting_edges.clear();
    ps_edge_to_vertices.clear(); // free

    ///////////////////////////////////////////////////////////////////////////
    // Polygon tracing (clipping of intersecting polygon-soup faces)
    ///////////////////////////////////////////////////////////////////////////

    // Now we start to clip every intersecting face
    // -----------------------------------------------

    TIMESTACK_PUSH("Clip polygons"); // &&&&&

    // Stores the all polygons, including new polygons that are produced after clipping
    // and the faces that remained unchanged because they were not intersecting. Note
    // that faces in the polygon soup that were found to be intersecting are replaced
    // with "child" faces that result from their clipping.
    //
    // I use the word "polygon" here because they are not yet used to define a mesh -
    // at which point they become faces!
    std::vector<traced_polygon_t> m0_polygons;

    // m0 polygons adjacent to cutpath from source-mesh faces
    std::vector<int> m0_sm_cutpath_adjacent_polygons;
    // m0 polygons adjacent to cutpath from cut-mesh faces
    std::vector<int> m0_cm_cutpath_adjacent_polygons;

    int traced_sm_polygon_count = 0;

    std::unordered_map<int, fd_t> m0_to_ps_face; // (we'll later also include reversed polygon patches)

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        typedef face_array_iterator_t InputStorageIteratorType;
        typedef std::tuple<
            std::vector<traced_polygon_t>, // m0_polygons;
            std::vector<int>, // m0_sm_cutpath_adjacent_polygons
            std::vector<int>, // m0_cm_cutpath_adjacent_polygons;
            int, // traced_sm_polygon_count
            std::unordered_map<int, fd_t>> // m0_to_ps_face
            OutputStorageTypesTuple;

        auto fn_trace_polygons = [&](
                                     InputStorageIteratorType block_start_,
                                     InputStorageIteratorType block_end_) -> OutputStorageTypesTuple {
            OutputStorageTypesTuple local_output;
            const uint32_t num_elems = (uint32_t)std::distance(block_start_, block_end_);
            std::vector<traced_polygon_t>& m0_polygons_LOCAL = std::get<0>(local_output);
            m0_polygons_LOCAL.reserve(num_elems);
            std::vector<int>& m0_sm_cutpath_adjacent_polygons_LOCAL = std::get<1>(local_output);
            m0_sm_cutpath_adjacent_polygons_LOCAL.reserve(ps_iface_to_m0_edge_list.size());
            std::vector<int>& m0_cm_cutpath_adjacent_polygons_LOCAL = std::get<2>(local_output);
            m0_cm_cutpath_adjacent_polygons_LOCAL.reserve(ps_iface_to_m0_edge_list.size());
            int& traced_sm_polygon_count_LOCAL = std::get<3>(local_output);
            std::unordered_map<int, fd_t>& m0_to_ps_face_LOCAL = std::get<4>(local_output);
            m0_to_ps_face_LOCAL.reserve(num_elems);
            traced_sm_polygon_count_LOCAL = 0;

            for (face_array_iterator_t ps_face_iter = block_start_; ps_face_iter != block_end_; ++ps_face_iter) {
                const fd_t& ps_face = *ps_face_iter;

                // get all the edges that lie on "ps_face", including the new one after partiting acording to intersection
                // points, and the old one which did not intersect any face
                std::unordered_map<fd_t, std::vector<ed_t>>::iterator ps_iface_to_m0_edge_list_fiter = ps_iface_to_m0_edge_list.find(ps_face);

                bool is_intersecting_ps_face = ps_iface_to_m0_edge_list_fiter != ps_iface_to_m0_edge_list.end();
                bool is_from_cut_mesh = ps_is_cutmesh_face(ps_face, sm_face_count);

                std::vector<traced_polygon_t> child_polygons; // new polygons traced on current face

                if (is_intersecting_ps_face == false) { // non-intersecting face

                    traced_polygon_t retraced_poly; // ordered sequence of halfedges defining the unchanged polygon
                    std::vector<hd_t> halfedges_around_face = ps.get_halfedges_around_face(ps_face);
                    retraced_poly.reserve(halfedges_around_face.size()); // minimum 3 (triangle)

                    // for each halfedge in the current polygon
                    for (std::vector<hd_t>::const_iterator hbegin = halfedges_around_face.cbegin(); hbegin != halfedges_around_face.cend(); ++hbegin) {
                        // get the source and target vertex descriptors in the polygon soup
                        const vd_t ps_h_src = ps.source(*hbegin);
                        const vd_t ps_h_tgt = ps.target(*hbegin);

                        MCUT_ASSERT((int)(ps_h_src) < (int)ps_to_m0_vtx.size());
                        const vd_t m0_h_src = SAFE_ACCESS(ps_to_m0_vtx, ps_h_src);
                        MCUT_ASSERT((int)(ps_h_tgt) < (int)ps_to_m0_vtx.size());
                        const vd_t m0_h_tgt = SAFE_ACCESS(ps_to_m0_vtx, ps_h_tgt);

                        const ed_t ps_edge = ps.edge(*hbegin);
                        const ed_t m0_edge = SAFE_ACCESS(ps_to_m0_non_intersecting_edge, ps_edge);
                        const hd_t m0_edge_h0 = m0.halfedge(m0_edge, 0);
                        const hd_t m0_edge_h1 = m0.halfedge(m0_edge, 1);

                        // resolve the correct halfedge by match the source and target vertex descriptors
                        if (m0.source(m0_edge_h0) == m0_h_src && m0.target(m0_edge_h0) == m0_h_tgt) {
                            retraced_poly.emplace_back(m0_edge_h0);
                        } else {
                            retraced_poly.emplace_back(m0_edge_h1);
                        }
                    }

                    MCUT_ASSERT(retraced_poly.size() == halfedges_around_face.size());

                    const int poly_idx = (int)(m0_polygons_LOCAL.size() + child_polygons.size());
                    m0_to_ps_face_LOCAL[poly_idx] = ps_face;

                    child_polygons.emplace_back(retraced_poly);
                } else {

                    // Here we enter the complex case of having to actually clip the current face
                    // because it is intersecting
                    // --------------------------------------------------------------------------

                    const std::vector<ed_t>& ps_iface_m0_edge_list = ps_iface_to_m0_edge_list_fiter->second;
                    static thread_local std::vector<vd_t> ps_coincident_vertices_tmp;

                    ps.get_vertices_around_face(ps_coincident_vertices_tmp, ps_face);
                    const std::vector<vd_t>& ps_coincident_vertices = ps_coincident_vertices_tmp;
                    static thread_local std::vector<vd_t> coincident_vertices; // "m0" versions of those stored in "ps_coincident_vertices"
                    coincident_vertices.resize(ps_coincident_vertices.size());
                    for (int i = 0; i < (int)ps_coincident_vertices.size(); ++i) {
                        const vd_t ps_v = ps_coincident_vertices[i];
                        MCUT_ASSERT((int)(ps_v) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_v) != ps_to_m0_vtx.cend()*/);
                        const vd_t m0_v = ps_to_m0_vtx[ps_v];
                        coincident_vertices[i] = m0_v;
                    }

                    MCUT_ASSERT(coincident_vertices.size() == ps_coincident_vertices.size());

                    const std::unordered_map<fd_t, std::vector<vd_t>>::const_iterator ireg_entry_iter = ps_iface_to_ivtx_list.find(ps_face);

                    MCUT_ASSERT(ireg_entry_iter != ps_iface_to_ivtx_list.cend());

                    const std::vector<vd_t>& intersection_points_on_face = ireg_entry_iter->second;
                    coincident_vertices.reserve(coincident_vertices.size() + intersection_points_on_face.size());
                    coincident_vertices.insert(coincident_vertices.end(), intersection_points_on_face.cbegin(), intersection_points_on_face.cend());

                    MCUT_ASSERT(intersection_points_on_face.size() >= 2); // minimum (two intersecting convex polygons)

                    std::vector<ed_t> incident_edges = ps_iface_m0_edge_list; // COPY because "incident_edges" will be modified later
                    int incident_boundary_edge_count = 0;

                    std::partition(incident_edges.begin(), incident_edges.end(),
                        [&](const ed_t& e) {
                            const vd_t v0 = m0.vertex(e, 0);
                            const vd_t v1 = m0.vertex(e, 1);
                            const bool v0_is_ivtx = m0_is_intersection_point(v0, ps_vtx_cnt);
                            const bool v1_is_ivtx = m0_is_intersection_point(v1, ps_vtx_cnt);
                            const bool is_ambiguious_boundary_edge_case = (v0_is_ivtx && v1_is_ivtx);
                            bool is_valid_ambiguious_boundary_edge = false;

                            if (is_ambiguious_boundary_edge_case) {
                                MCUT_ASSERT((size_t)v0 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(v0) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                const std::pair<ed_t, fd_t>& v0_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)v0 - ps_vtx_cnt);
                                const ed_t v0_ps_edge = v0_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, v0); // ps.edge(v0_coincident_ps_halfedge);

                                MCUT_ASSERT((size_t)v1 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(v1) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                const std::pair<ed_t, fd_t>& v1_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)v1 - ps_vtx_cnt);
                                const ed_t v1_ps_edge = v1_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, v1); // ps.edge(v1_coincident_ps_halfedge);

                                is_valid_ambiguious_boundary_edge = (v0_ps_edge == v1_ps_edge);
                            }

                            bool is_boundary_edge = (!is_ambiguious_boundary_edge_case || is_valid_ambiguious_boundary_edge);
                            if (is_boundary_edge) {
                                incident_boundary_edge_count++; // count
                            }
                            return is_boundary_edge;
                        });

                    MCUT_ASSERT(incident_boundary_edge_count >= 3); // minimum is 3 edge which is for a triangle

                    // const int interior_edges_on_face = (int)incident_edges.size() - incident_boundary_edge_count;
                    std::vector<hd_t> incident_halfedges;
                    hd_t first_boundary_halfedge = hmesh_t::null_halfedge();

                    for (std::vector<ed_t>::const_iterator incident_edge_iter = incident_edges.cbegin();
                         incident_edge_iter != incident_edges.cbegin() + incident_boundary_edge_count;
                         ++incident_edge_iter) {
                        const ed_t& edge = (*incident_edge_iter);

                        for (int edge_he_iter = 0; edge_he_iter < 2; ++edge_he_iter) {
                            const hd_t m0_edge_he = m0.halfedge(edge, edge_he_iter);
                            const vd_t m0_edge_he_src = m0.source(m0_edge_he);
                            const vd_t m0_edge_he_tgt = m0.target(m0_edge_he);
                            const bool m0_edge_he_src_is_ivertex = m0_is_intersection_point(m0_edge_he_src, ps_vtx_cnt);
                            const bool m0_edge_he_tgt_is_ivertex = m0_is_intersection_point(m0_edge_he_tgt, ps_vtx_cnt);

                            if (!m0_edge_he_src_is_ivertex && !m0_edge_he_tgt_is_ivertex) { // o-->o (unmodified original edge)

                                const vd_t ps_he_src = SAFE_ACCESS(m0_to_ps_vtx, m0_edge_he_src);
                                const vd_t ps_he_tgt = SAFE_ACCESS(m0_to_ps_vtx, m0_edge_he_tgt);
                                const hd_t ps_he = ps.halfedge(ps_he_src, ps_he_tgt);

                                if (ps_he == hmesh_t::null_halfedge()) {
                                    continue;
                                }

                                if (ps.face(ps_he) == ps_face) {
                                    first_boundary_halfedge = m0_edge_he;
                                    break;
                                }
                            } else { // x-->x OR o-->x OR x-->o
                                const bool is_ox = (!m0_edge_he_src_is_ivertex && m0_edge_he_tgt_is_ivertex);

                                if (is_ox) {
                                    MCUT_ASSERT((size_t)m0_edge_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_edge_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                    const std::pair<ed_t, fd_t>& m0_edge_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)m0_edge_he_tgt - ps_vtx_cnt);

                                    // get the incident ps-halfedge of tgt
                                    ed_t tgt_ps_e = m0_edge_he_tgt_ipair.first;
                                    hd_t tgt_ps_h = ps.halfedge(tgt_ps_e, 0);

                                    if (ps.face(tgt_ps_h) != ps_face) {
                                        tgt_ps_h = ps.opposite(tgt_ps_h);
                                        MCUT_ASSERT(tgt_ps_h != hmesh_t::null_halfedge());
                                    }

                                    const vd_t& m0_edge_he_src_as_ps_vertex = SAFE_ACCESS(m0_to_ps_vtx, m0_edge_he_src);

                                    if (m0_edge_he_src_as_ps_vertex == ps.source(tgt_ps_h)) { // is counter clock-wise halfedge
                                        first_boundary_halfedge = m0_edge_he;
                                        break;
                                    }
                                } else { // x-->x OR x-->o

                                    const bool is_xx = m0_edge_he_src_is_ivertex && m0_edge_he_tgt_is_ivertex;

                                    if (is_xx) { // exterior interior-iedge
                                        MCUT_ASSERT((size_t)m0_edge_he_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_edge_he_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                        const std::pair<ed_t, fd_t>& m0_edge_he_src_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)m0_edge_he_src - ps_vtx_cnt);
                                        MCUT_ASSERT((size_t)m0_edge_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_edge_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                        const std::pair<ed_t, fd_t>& m0_edge_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)m0_edge_he_tgt - ps_vtx_cnt);

                                        const ed_t src_ps_edge = m0_edge_he_src_ipair.first; // ps.edge(src_coincident_ps_halfedge)
                                        const ed_t tgt_ps_edge = m0_edge_he_tgt_ipair.first; // ps.edge(tgt_ps_h);
                                        const bool is_exterior_ih = (src_ps_edge == tgt_ps_edge);

                                        if (!is_exterior_ih) {
                                            continue; // interior ihalfedges cannot be used as the first exterior ihalfedge
                                        }

                                        hd_t ps_halfedge_of_face = ps.halfedge(tgt_ps_edge, 0); // tgt_ps_h;

                                        if (ps.face(ps_halfedge_of_face) != ps_face) {
                                            ps_halfedge_of_face = ps.opposite(ps_halfedge_of_face);
                                            MCUT_ASSERT(ps_halfedge_of_face != hmesh_t::null_halfedge()); // guarranteed to exist since we have a poly-boundary interior ihalfedge
                                        }

                                        const ed_t& incident_ps_edge = ps.edge(ps_halfedge_of_face);
                                        std::map<ed_t, std::vector<ed_t>>::const_iterator ps_to_m0_edges_find_iter = ps_to_m0_edges.find(incident_ps_edge);
                                        MCUT_ASSERT(ps_to_m0_edges_find_iter != ps_to_m0_edges.cend()); // because incident_ps_edge contains a polygon exterior interior ihalfedge

                                        const std::vector<ed_t>& sorted_m0_edges = ps_to_m0_edges_find_iter->second;
                                        std::vector<hd_t> halfedge_sequence;
                                        const ed_t& first_e = sorted_m0_edges.front();
                                        hd_t first_he = m0.halfedge(first_e, 0);
                                        vd_t first_he_src = m0.source(first_he);

                                        if (m0_is_intersection_point(first_he_src, ps_vtx_cnt)) {
                                            // TODO: I think this scope is never entered based on how we created "sorted_m0_edges".
                                            // Thus, the source vertex of the first halfedge in the sequence cannot be an intersection point
                                            first_he = m0.halfedge(first_e, 1);
                                            first_he_src = m0.source(first_he);
                                            MCUT_ASSERT(!m0_is_intersection_point(first_he_src, ps_vtx_cnt)); // expect original vertex since halfedge edge is the first in sequence
                                        }

                                        halfedge_sequence.push_back(first_he);

                                        for (std::vector<ed_t>::const_iterator seq_edge_iter = sorted_m0_edges.cbegin() + 1; // we have already added the first halfedge
                                             seq_edge_iter != sorted_m0_edges.cend(); ++seq_edge_iter) {
                                            const ed_t& e = *seq_edge_iter;
                                            const hd_t h0 = m0.halfedge(e, 0);

                                            if (m0.source(h0) == m0.target(halfedge_sequence.back())) {
                                                halfedge_sequence.push_back(h0);
                                            } else {
                                                const hd_t h1 = m0.halfedge(e, 1);
                                                halfedge_sequence.push_back(h1);
                                            }
                                        }

                                        MCUT_ASSERT(halfedge_sequence.size() == sorted_m0_edges.size());

                                        const vd_t& first_he_src_as_ps_vertex = SAFE_ACCESS(m0_to_ps_vtx, first_he_src); // first he of sequence

                                        if (first_he_src_as_ps_vertex != ps.source(ps_halfedge_of_face)) {
                                            std::for_each(
                                                halfedge_sequence.begin(),
                                                halfedge_sequence.end(),
                                                [&](hd_t& he) {
                                                    he = m0.opposite(he);
                                                }); // flip seq
                                        }

                                        std::vector<hd_t>::const_iterator matching_he_find_iter = std::find_if(
                                            halfedge_sequence.cbegin(),
                                            halfedge_sequence.cend(),
                                            [&](const hd_t& he) {
                                                const vd_t& he_src = m0.source(he);
                                                const vd_t& he_tgt = m0.target(he);

                                                return (he_src == m0_edge_he_src && he_tgt == m0_edge_he_tgt /*|| (he_src == m0_edge_he_tgt && he_tgt == m0_edge_he_src*/);
                                            });

                                        MCUT_ASSERT(matching_he_find_iter != halfedge_sequence.cend()); // does the potential halfedge actually point in the correct direction or not
                                        {
                                            first_boundary_halfedge = *matching_he_find_iter;
                                            break;
                                        }
                                    } // if (is_xx) {
                                    else {
                                    }
                                } // } else { // x-->x OR x-->o
                            } // } else { // x-->x OR o-->x OR x-->o
                        } // for (int edge_he_iter = 0; edge_he_iter < 2; ++edge_he_iter) {

                        if (first_boundary_halfedge != hmesh_t::null_halfedge()) {
                            break; // done
                        }
                    }

                    MCUT_ASSERT(first_boundary_halfedge != hmesh_t::null_halfedge());

                    hd_t current_exterior_halfedge = hmesh_t::null_halfedge();
                    hd_t next_exterior_halfedge = first_boundary_halfedge;
                    std::unordered_map<ed_t, bool> walked_edges;

                    do {

                        current_exterior_halfedge = next_exterior_halfedge;
                        incident_halfedges.push_back(current_exterior_halfedge);
                        walked_edges[m0.edge(current_exterior_halfedge)] = true;

                        const vd_t current_tgt = m0.target(current_exterior_halfedge);
                        next_exterior_halfedge = hmesh_t::null_halfedge(); // reset

                        // find next boundary halfedge from incident edges
                        for (std::vector<ed_t>::const_iterator incident_edge_iter = incident_edges.cbegin();
                             incident_edge_iter != incident_edges.cbegin() + incident_boundary_edge_count; // we only want exterior edges;
                             ++incident_edge_iter) {
                            const ed_t& edge = *incident_edge_iter;
                            bool edge_walked = walked_edges.find(edge) != walked_edges.cend(); // std::find(walked_edges.cbegin(), walked_edges.cend(), edge) != walked_edges.cend();

                            if (edge_walked) {
                                continue; // skip edge is walked already
                            }

                            const vd_t v0 = m0.vertex(edge, 0);
                            const vd_t v1 = m0.vertex(edge, 1);

                            if (v0 == current_tgt || v1 == current_tgt) // check if connected to current (i.e. they share one vertex)
                            {
                                const bool v0_is_ivtx = m0_is_intersection_point(v0, ps_vtx_cnt);
                                const bool v1_is_ivtx = m0_is_intersection_point(v1, ps_vtx_cnt);
                                bool is_ambiguious_boundary_edge_case = v0_is_ivtx && v1_is_ivtx;
                                bool is_valid_ambiguious_boundary_edge = false;

                                if (is_ambiguious_boundary_edge_case) { // exterior edge with two intersection vertices (ambigious case arising from concave polyhedron cut)

                                    MCUT_ASSERT((size_t)v0 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(v0) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                    const std::pair<ed_t, fd_t>& v0_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)v0 - ps_vtx_cnt);
                                    const ed_t v0_ps_edge = v0_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, v0); //ps.edge(v0_coincident_ps_halfedge);

                                    MCUT_ASSERT((size_t)v1 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(v1) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                    const std::pair<ed_t, fd_t>& v1_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)v1 - ps_vtx_cnt);
                                    const ed_t v1_ps_edge = v1_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, v1); // ps.edge(v1_coincident_ps_halfedge);

                                    is_valid_ambiguious_boundary_edge = (v0_ps_edge == v1_ps_edge); // see also above when gathering exterior incident edges
                                }

                                if (!is_ambiguious_boundary_edge_case || is_valid_ambiguious_boundary_edge) {
                                    const hd_t h0 = m0.halfedge(edge, 0);
                                    const hd_t h1 = m0.halfedge(edge, 1);

                                    next_exterior_halfedge = h0;

                                    if (m0.source(h0) != current_tgt) // h0 is facing the opposite dir
                                    {
                                        next_exterior_halfedge = h1;
                                    }

                                    break; // found
                                }
                            }
                        }
                    } while (next_exterior_halfedge != hmesh_t::null_halfedge() /*first_boundary_halfedge*/);

                    MCUT_ASSERT(incident_halfedges.size() >= 3); // minimum i.e. for a triangles!

                    const int exterior_halfedge_count = (int)incident_halfedges.size();

                    MCUT_ASSERT(exterior_halfedge_count == incident_boundary_edge_count);

                    for (std::vector<ed_t>::const_iterator incident_edge_iter = incident_edges.cbegin() + incident_boundary_edge_count; // start from polygon interior edges offset
                         incident_edge_iter != incident_edges.cend();
                         ++incident_edge_iter) {
                        const ed_t& edge = (*incident_edge_iter);
                        const hd_t h0 = m0.halfedge(edge, 0);
                        const hd_t h1 = m0.halfedge(edge, 1);
                        incident_halfedges.push_back(h0);
                        incident_halfedges.push_back(h1);
                    }

                    std::vector<hd_t> incident_halfedges_to_be_walked(incident_halfedges.cbegin(), incident_halfedges.cend()); // copy

                    do { // each iteration traces a child polygon

                        traced_polygon_t child_polygon;
                        hd_t current_halfedge = hmesh_t::null_halfedge();
                        hd_t next_halfedge = incident_halfedges_to_be_walked.front();

                        MCUT_ASSERT(incident_halfedges_to_be_walked.size() >= 2);

                        bool is_valid_polygon = false;
                        do { // each iteration walks a halfedge to incremetally trace a child polygon

                            current_halfedge = next_halfedge;
                            child_polygon.push_back(current_halfedge);
                            const vd_t current_halfedge_target = m0.target(current_halfedge);
                            next_halfedge = hmesh_t::null_halfedge(); // reset

                            {
                                std::vector<hd_t>::iterator find_iter = std::find(incident_halfedges_to_be_walked.begin(), incident_halfedges_to_be_walked.end(), current_halfedge);
                                MCUT_ASSERT(find_iter != incident_halfedges_to_be_walked.cend());
                                incident_halfedges_to_be_walked.erase(find_iter); // remove
                            }

                            if (child_polygon.size() >= 3) { // minimum halfedge count to constitute a valid polygon (triangle)

                                // source of first halfedge is target of last
                                if (m0.source(child_polygon.front()) == m0.target(child_polygon.back())) {
                                    if (current_halfedge != m0.opposite(child_polygon.front())) {
                                        is_valid_polygon = true;
                                        break;
                                    } else { // ... if the current halfedge is the opposite of the first halfedge in list
                                        std::vector<hd_t> premptive_candidate_halfedges;
                                        for (std::vector<hd_t>::const_iterator incident_halfedges_to_be_walked_iter = incident_halfedges_to_be_walked.cbegin(); incident_halfedges_to_be_walked_iter != incident_halfedges_to_be_walked.cend(); ++incident_halfedges_to_be_walked_iter) {
                                            const hd_t& potential_candidate = *incident_halfedges_to_be_walked_iter;
                                            if (m0.source(potential_candidate) == current_halfedge_target) {
                                                premptive_candidate_halfedges.push_back(potential_candidate);
                                            }
                                        }

                                        hd_t prime_candidate = hmesh_t::null_halfedge();
                                        if (premptive_candidate_halfedges.size() == 2) {

                                            const std::vector<hd_t>::const_iterator current_halfedge_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                                            MCUT_ASSERT(current_halfedge_find_iter != incident_halfedges.cend());
                                            const bool current_halfedge_is_exterior = std::distance(incident_halfedges.cbegin(), current_halfedge_find_iter) < exterior_halfedge_count;

                                            if (current_halfedge_is_exterior) {

                                                // pick interior candidate
                                                const std::vector<hd_t>::const_iterator prime_candidate_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                                                MCUT_ASSERT(prime_candidate_find_iter != incident_halfedges.cend());
                                                const bool prime_candidate_is_exterior = std::distance(incident_halfedges.cbegin(), prime_candidate_find_iter) < exterior_halfedge_count;

                                                if (prime_candidate_is_exterior) {
                                                    prime_candidate = premptive_candidate_halfedges.back(); // select correct interior halfedge
                                                }
                                            } else { // interior

                                                // pick non-opposite
                                                const bool prime_candidate_is_opposite = m0.opposite(current_halfedge) == prime_candidate;

                                                if (prime_candidate_is_opposite) {
                                                    prime_candidate = premptive_candidate_halfedges.back(); // select correct non-opposite halfedge
                                                }
                                            }
                                        }

                                        const hd_t premptive_next = prime_candidate;

                                        MCUT_ASSERT(premptive_candidate_halfedges.size() <= 2);

                                        if (std::find(child_polygon.cbegin(), child_polygon.cend(), premptive_next) != child_polygon.cend()) {
                                            is_valid_polygon = true;
                                            break;
                                        }
                                    }
                                }
                            }

                            std::vector<hd_t> candidate_halfedges;
                            candidate_halfedges.reserve(2); // two candidates at most because we filtered out exterior interior-halfedges as well as clockwise (cw) halfedge

                            for (std::vector<hd_t>::const_iterator incident_halfedges_to_be_walked_iter = incident_halfedges_to_be_walked.cbegin(); incident_halfedges_to_be_walked_iter != incident_halfedges_to_be_walked.cend(); ++incident_halfedges_to_be_walked_iter) {
                                const hd_t& potential_candidate = *incident_halfedges_to_be_walked_iter;
                                if (m0.source(potential_candidate) == current_halfedge_target) {
                                    candidate_halfedges.push_back(potential_candidate);
                                }
                            }

                            MCUT_ASSERT(candidate_halfedges.size() <= 2);

                            // 2.2. select prime candidate
                            hd_t prime_candidate = hmesh_t::null_halfedge();

                            if (!candidate_halfedges.empty()) {
                                prime_candidate = candidate_halfedges.front(); // assuming: candidate_halfedges.size() == 1
                            }

                            if (candidate_halfedges.size() == 2) {

                                const std::vector<hd_t>::const_iterator current_halfedge_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                                MCUT_ASSERT(current_halfedge_find_iter != incident_halfedges.cend());
                                const bool current_halfedge_is_exterior = std::distance(incident_halfedges.cbegin(), current_halfedge_find_iter) < exterior_halfedge_count;

                                if (current_halfedge_is_exterior) {
                                    // pick interior candidate
                                    const std::vector<hd_t>::const_iterator prime_candidate_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                                    MCUT_ASSERT(prime_candidate_find_iter != incident_halfedges.cend());
                                    const bool prime_candidate_is_exterior = std::distance(incident_halfedges.cbegin(), prime_candidate_find_iter) < exterior_halfedge_count;

                                    if (prime_candidate_is_exterior) {
                                        prime_candidate = candidate_halfedges.back(); // select correct interior halfedge
                                    }
                                } else { // interior
                                    const bool prime_candidate_is_opposite = m0.opposite(current_halfedge) == prime_candidate;

                                    if (prime_candidate_is_opposite) {
                                        prime_candidate = candidate_halfedges.back(); // select correct non-opposite halfedge
                                    }
                                }
                            }

                            next_halfedge = prime_candidate;
                        } while (next_halfedge != hmesh_t::null_halfedge());

                        if (is_valid_polygon) {

                            const int poly_idx = (int)(m0_polygons_LOCAL.size() + child_polygons.size());
                            if (ps_is_cutmesh_face(ps_face, sm_face_count)) {
                                m0_cm_cutpath_adjacent_polygons_LOCAL.push_back(poly_idx);
                            } else {
                                m0_sm_cutpath_adjacent_polygons_LOCAL.push_back(poly_idx);
                            }

                            m0_to_ps_face_LOCAL[poly_idx] = ps_face;
                            child_polygons.emplace_back(child_polygon);
                        }

                    } while (!incident_halfedges_to_be_walked.empty());
                } // if (!is_intersecting_ps_face) {

                m0_polygons_LOCAL.insert(m0_polygons_LOCAL.end(), child_polygons.cbegin(), child_polygons.cend());

                if (!is_from_cut_mesh) {
                    traced_sm_polygon_count_LOCAL += (int)child_polygons.size();
                }
            }

            return local_output;
        };

        std::vector<std::future<OutputStorageTypesTuple>> futures;
        OutputStorageTypesTuple partial_res;

        parallel_for(
            *input.scheduler,
            ps.faces_begin(),
            ps.faces_end(),
            fn_trace_polygons,
            partial_res, // output computed by master thread
            futures);

        // This lambda merges the local traced face data structures computed by each
        // thread into their corresponding global data structure.
        auto merge_local_traced_faces = [](
                                            const std::vector<traced_polygon_t>& m0_polygons_FUTURE,
                                            const std::vector<int>& m0_sm_cutpath_adjacent_polygons_FUTURE,
                                            const std::vector<int>& m0_cm_cutpath_adjacent_polygons_FUTURE,
                                            const int& traced_sm_polygon_count_FUTURE,
                                            const std::unordered_map<int, fd_t>& m0_to_ps_face_FUTURE,
                                            std::vector<traced_polygon_t>& m0_polygons,
                                            std::vector<int>& m0_sm_cutpath_adjacent_polygons,
                                            std::vector<int>& m0_cm_cutpath_adjacent_polygons,
                                            int& traced_sm_polygon_count,
                                            std::unordered_map<int, fd_t>& m0_to_ps_face) {
            int base_offset = (int)m0_polygons.size();
            m0_polygons.reserve(m0_polygons.size() + m0_polygons_FUTURE.size());
            m0_polygons.insert(m0_polygons.end(), m0_polygons_FUTURE.cbegin(), m0_polygons_FUTURE.cend());

            m0_sm_cutpath_adjacent_polygons.reserve(m0_sm_cutpath_adjacent_polygons.size() + m0_sm_cutpath_adjacent_polygons_FUTURE.size());
            for (int i = 0; i < (int)m0_sm_cutpath_adjacent_polygons_FUTURE.size(); ++i) {
                const int local_polygon_idx = m0_sm_cutpath_adjacent_polygons_FUTURE[i];
                const int global_polygon_idx = local_polygon_idx + base_offset;
                m0_sm_cutpath_adjacent_polygons.push_back(global_polygon_idx);
            }

            m0_cm_cutpath_adjacent_polygons.reserve(m0_cm_cutpath_adjacent_polygons.size() + m0_cm_cutpath_adjacent_polygons_FUTURE.size());
            for (int i = 0; i < (int)m0_cm_cutpath_adjacent_polygons_FUTURE.size(); ++i) {
                const int local_polygon_idx = m0_cm_cutpath_adjacent_polygons_FUTURE[i];
                const int global_polygon_idx = local_polygon_idx + base_offset;
                m0_cm_cutpath_adjacent_polygons.push_back(global_polygon_idx);
            }

            traced_sm_polygon_count += traced_sm_polygon_count_FUTURE;

            for (std::unordered_map<int, fd_t>::const_iterator i = m0_to_ps_face_FUTURE.cbegin(); i != m0_to_ps_face_FUTURE.cend(); ++i) {
                const int local_polygon_idx = i->first;
                const int global_polygon_idx = local_polygon_idx + base_offset;
                m0_to_ps_face[global_polygon_idx] = i->second;
            }
        };

        // merge thread-local output into global data structures
        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<OutputStorageTypesTuple>& f = futures[i];
            MCUT_ASSERT(f.valid());
            OutputStorageTypesTuple future_result = f.get(); // "get()" is a blocking function

            const std::vector<traced_polygon_t>& m0_polygons_FUTURE = std::get<0>(future_result);
            const std::vector<int>& m0_sm_cutpath_adjacent_polygons_FUTURE = std::get<1>(future_result);
            const std::vector<int>& m0_cm_cutpath_adjacent_polygons_FUTURE = std::get<2>(future_result);
            const int& traced_sm_polygon_count_FUTURE = std::get<3>(future_result);
            const std::unordered_map<int, fd_t>& m0_to_ps_face_FUTURE = std::get<4>(future_result);

            merge_local_traced_faces(
                m0_polygons_FUTURE,
                m0_sm_cutpath_adjacent_polygons_FUTURE,
                m0_cm_cutpath_adjacent_polygons_FUTURE,
                traced_sm_polygon_count_FUTURE,
                m0_to_ps_face_FUTURE,
                m0_polygons,
                m0_sm_cutpath_adjacent_polygons,
                m0_cm_cutpath_adjacent_polygons,
                traced_sm_polygon_count,
                m0_to_ps_face);
        }

        // merge master thread output at the end to that we maintain the order of the traced polygons
        // This order is important for a number of tricks employed throughout the code in following parts
        // e.g. using integer offsets to infer the start of cm traced polygons etc.
        const std::vector<traced_polygon_t>& m0_polygons_MASTER_THREAD_LOCAL = std::get<0>(partial_res);
        const std::vector<int>& m0_sm_cutpath_adjacent_polygons_MASTER_THREAD_LOCAL = std::get<1>(partial_res);
        const std::vector<int>& m0_cm_cutpath_adjacent_polygons_MASTER_THREAD_LOCAL = std::get<2>(partial_res);
        const int& traced_sm_polygon_count_MASTER_THREAD_LOCAL = std::get<3>(partial_res);
        const std::unordered_map<int, fd_t>& m0_to_ps_face_MASTER_THREAD_LOCAL = std::get<4>(partial_res);

        merge_local_traced_faces(
            m0_polygons_MASTER_THREAD_LOCAL,
            m0_sm_cutpath_adjacent_polygons_MASTER_THREAD_LOCAL,
            m0_cm_cutpath_adjacent_polygons_MASTER_THREAD_LOCAL,
            traced_sm_polygon_count_MASTER_THREAD_LOCAL,
            m0_to_ps_face_MASTER_THREAD_LOCAL,
            m0_polygons,
            m0_sm_cutpath_adjacent_polygons,
            m0_cm_cutpath_adjacent_polygons,
            traced_sm_polygon_count,
            m0_to_ps_face);
    } // end of parallel scope
#else
    // for each face in the polygon-soup mesh
    for (face_array_iterator_t ps_face_iter = ps.faces_begin(); ps_face_iter != ps.faces_end(); ++ps_face_iter) {

        const fd_t& ps_face = *ps_face_iter;

        // get all the edges that lie on "ps_face", including the new one after partiting acording to intersection
        // points, and the old one which did not intersect any face
        std::unordered_map<fd_t, std::vector<ed_t>>::iterator ps_iface_to_m0_edge_list_fiter = ps_iface_to_m0_edge_list.find(ps_face);

        bool is_intersecting_ps_face = ps_iface_to_m0_edge_list_fiter != ps_iface_to_m0_edge_list.end();
        bool is_from_cut_mesh = ps_is_cutmesh_face(ps_face, sm_face_count);

        std::vector<traced_polygon_t> child_polygons; // new polygons traced on current face

        if (is_intersecting_ps_face == false) { // non-intersecting face

            // NOTE: here we just copy the polygon as-is because it does not change.
            // --------------------------------------------------------------------

            traced_polygon_t retraced_poly; // ordered sequence of halfedges defining the unchanged polygon

            // query the halfedge sequence in the polygon soup that defines our polygon
            std::vector<hd_t> halfedges_around_face = ps.get_halfedges_around_face(ps_face);

            retraced_poly.reserve(halfedges_around_face.size()); // minimum 3 (triangle)

            // what we are going to do now is:
            //  for each halfedge in "halfedges_around_face" (ps), find its equivalent halfedge in "m0"
            //  The found m0-halfedges will then define "retraced_poly"
            // ----------------------------------------------------------------------------------------

            // for each halfedge in the current polygon
            for (std::vector<hd_t>::const_iterator hbegin = halfedges_around_face.cbegin(); hbegin != halfedges_around_face.cend(); ++hbegin) {

                // get the source and target vertex descriptors in the polygon soup
                const vd_t ps_h_src = ps.source(*hbegin);
                const vd_t ps_h_tgt = ps.target(*hbegin);

                MCUT_ASSERT((int)(ps_h_src) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_h_src) != ps_to_m0_vtx.cend()*/);
                const vd_t m0_h_src = SAFE_ACCESS(ps_to_m0_vtx, ps_h_src);
                MCUT_ASSERT((int)(ps_h_tgt) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_h_tgt) != ps_to_m0_vtx.cend()*/);
                const vd_t m0_h_tgt = SAFE_ACCESS(ps_to_m0_vtx, ps_h_tgt);

                // Now we find the actual "m0" halfedge equivalent to "*hbegin" using
                // our "m0" source and target descriptors
                // --------------------------------------------------------------------

                const ed_t ps_edge = ps.edge(*hbegin); // polygon soup version of the edge of the current halfedge
                const ed_t m0_edge = SAFE_ACCESS(ps_to_m0_non_intersecting_edge, ps_edge); // "m0" version of edge
                const hd_t m0_edge_h0 = m0.halfedge(m0_edge, 0);
                const hd_t m0_edge_h1 = m0.halfedge(m0_edge, 1);

                // resolve the correct halfedge by match the source and target vertex descriptors
                if (m0.source(m0_edge_h0) == m0_h_src && m0.target(m0_edge_h0) == m0_h_tgt) {

                    retraced_poly.emplace_back(m0_edge_h0);
                } else {

                    retraced_poly.emplace_back(m0_edge_h1);
                }
            }

            MCUT_ASSERT(retraced_poly.size() == halfedges_around_face.size());

            const int poly_idx = (int)(m0_polygons.size() + child_polygons.size());
            m0_to_ps_face[poly_idx] = ps_face;

            // save the retraced polygon, using the information in "m0" from "ps"
            child_polygons.emplace_back(retraced_poly);
        } else {

            // Here we enter the complex case of having to actually clip the current face
            // because it is intersecting
            // --------------------------------------------------------------------------

            // retrieve the list of edges which lie on the face (some new and some original)
            const std::vector<ed_t>& ps_iface_m0_edge_list = ps_iface_to_m0_edge_list_fiter->second;

            // Now we gather vertices on face (including intersection points)
            // ------------------------------------------------------

            // Get the original vertices first, which we do by first querying them from "ps"
            // and then using our maps to get their "m0" versions.
            static thread_local std::vector<vd_t> ps_coincident_vertices_tmp;
            ps.get_vertices_around_face(ps_coincident_vertices_tmp, ps_face);
            const std::vector<vd_t>& ps_coincident_vertices = ps_coincident_vertices_tmp;
            static thread_local std::vector<vd_t> coincident_vertices; // "m0" versions of those stored in "ps_coincident_vertices"
            coincident_vertices.resize(ps_coincident_vertices.size());
            // gather the original (m0) vertices on the face
            for (int i = 0; i < (int)ps_coincident_vertices.size(); ++i) {
                const vd_t ps_v = ps_coincident_vertices[i];

                MCUT_ASSERT((int)(ps_v) < (int)ps_to_m0_vtx.size() /*ps_to_m0_vtx.find(ps_v) != ps_to_m0_vtx.cend()*/);
                const vd_t m0_v = ps_to_m0_vtx[ps_v];

                coincident_vertices[i] = m0_v;
            }

            MCUT_ASSERT(coincident_vertices.size() == ps_coincident_vertices.size());

            // now we gather the intersection-points on the face
            const std::unordered_map<fd_t, std::vector<vd_t>>::const_iterator ireg_entry_iter = ps_iface_to_ivtx_list.find(ps_face);

            MCUT_ASSERT(ireg_entry_iter != ps_iface_to_ivtx_list.cend());

            // const int coincident_ps_vertex_count = (int)coincident_vertices.size();
            const std::vector<vd_t>& intersection_points_on_face = ireg_entry_iter->second;
            coincident_vertices.reserve(coincident_vertices.size() + intersection_points_on_face.size());
            coincident_vertices.insert(coincident_vertices.end(), intersection_points_on_face.cbegin(), intersection_points_on_face.cend());

            MCUT_ASSERT(intersection_points_on_face.size() >= 2); // minimum (two intersecting convex polygons)
#if 0
            // dump to log
            if (input.verbose) {

                for (std::vector<vd_t>::const_iterator j = coincident_vertices.cbegin(); j != coincident_vertices.cend(); ++j) {
                }
            }
#endif
            // After gathering the vertices above, we will now collect edges on the face
            // -------------------------------------------------------------------------

            // edges on face
            std::vector<ed_t> incident_edges = ps_iface_m0_edge_list; // COPY because "incident_edges" will be modified later

            // number of boundary edges on the face
            int incident_boundary_edge_count = 0;

            // We will now partition the list of incident edges into boundary/exterior and interior.
            // Boundary edges come first, then interior ones. We do this because it makes it easier for us
            // to filter our interior edges if they are consecutive in the list (i.e. in "incident_edges")
            std::partition(incident_edges.begin(), incident_edges.end(),
                [&](const ed_t& e) {
                    // calculate if edge is exterior
                    // -----------------------------

                    // get vertices defining edge
                    const vd_t v0 = m0.vertex(e, 0);
                    const vd_t v1 = m0.vertex(e, 1);

                    // are both vertices intersection points (the main property of interior edges)
                    const bool v0_is_ivtx = m0_is_intersection_point(v0, ps_vtx_cnt);
                    const bool v1_is_ivtx = m0_is_intersection_point(v1, ps_vtx_cnt);

                    // if both vertices are intersection points, we must be careful to make an extra
                    // check that the edge is really on the boundary. Moreover, it is possible that
                    // their exist an edge on the boundary whose vertices are both intersection
                    // points - hence the possible ambiguity.
                    // A boundary edge defined by two intersection points can arise from "carve-out"
                    // cuts..
                    const bool is_ambiguious_boundary_edge_case = (v0_is_ivtx && v1_is_ivtx);
                    bool is_valid_ambiguious_boundary_edge = false;

                    if (is_ambiguious_boundary_edge_case) {
                        // get their edges
                        MCUT_ASSERT((size_t)v0 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(v0) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t>& v0_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, v0 - ps_vtx_cnt);
                        const ed_t v0_ps_edge = v0_ipair.first; // m0_ivtx_to_ps_edge, v0); // ps.edge(v0_coincident_ps_halfedge);

                        MCUT_ASSERT((size_t)v1 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(v1) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t>& v1_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, v1 - ps_vtx_cnt);
                        const ed_t v1_ps_edge = v1_ipair.first; // m0_ivtx_to_ps_edge, v1); // ps.edge(v1_coincident_ps_halfedge);

                        // This is true if v0 and v1 where produced by multiple intersections of one edge
                        // with two different faces
                        is_valid_ambiguious_boundary_edge = (v0_ps_edge == v1_ps_edge);
                    }

                    bool is_boundary_edge = (!is_ambiguious_boundary_edge_case || is_valid_ambiguious_boundary_edge);
                    if (is_boundary_edge) {
                        incident_boundary_edge_count++; // count
                    }
                    return is_boundary_edge;
                });

            // dump info to log
#if 0
            if (lg.verbose()) {
                for (std::vector<ed_t>::const_iterator exterior_edge_iter = incident_edges.cbegin();
                     exterior_edge_iter != incident_edges.cbegin() + incident_boundary_edge_count;
                     ++exterior_edge_iter) {
                }
            }
#endif
            MCUT_ASSERT(incident_boundary_edge_count >= 3); // minimum is 3 edge which is for a triangle

            // const int interior_edges_on_face = (int)incident_edges.size() - incident_boundary_edge_count;

            //
            // We have the essential set of edges which will be used for clipping, the next step
            // is to gather the halfedges on the clipped face from these edges.
            //
            // Note that the gathered set of halfedges will contain some halfedges that are redundant.
            // These redundant halfedges are those which lie on the boundary of the clipped polygon and
            // have a winding order which is opposite to the winding order of the input mesh which contained
            // "ps_face" (i.e. either the cut-mesh or source-mesh).
            //
            // Thus, we need one more filtering step which will remove these redundant halfedges from
            // the gather set.

            std::vector<hd_t> incident_halfedges;

            // 1. find an exterior halfedge (any)
            hd_t first_boundary_halfedge = hmesh_t::null_halfedge();

            // for each edge on clipped polygon, (i.e. from the filtered set)
            for (std::vector<ed_t>::const_iterator incident_edge_iter = incident_edges.cbegin();
                 incident_edge_iter != incident_edges.cbegin() + incident_boundary_edge_count; // we only want exterior edges
                 ++incident_edge_iter) {
                const ed_t& edge = (*incident_edge_iter);

                // for each halfedge on the current edge
                for (int edge_he_iter = 0; edge_he_iter < 2; ++edge_he_iter) {

                    const hd_t m0_edge_he = m0.halfedge(edge, edge_he_iter);
                    const vd_t m0_edge_he_src = m0.source(m0_edge_he);
                    const vd_t m0_edge_he_tgt = m0.target(m0_edge_he);
                    const bool m0_edge_he_src_is_ivertex = m0_is_intersection_point(m0_edge_he_src, ps_vtx_cnt);
                    const bool m0_edge_he_tgt_is_ivertex = m0_is_intersection_point(m0_edge_he_tgt, ps_vtx_cnt);

                    if (!m0_edge_he_src_is_ivertex && !m0_edge_he_tgt_is_ivertex) { // o-->o (unmodified original edge)

                        const vd_t ps_he_src = m0_to_ps_vtx[m0_edge_he_src];
                        const vd_t ps_he_tgt = m0_to_ps_vtx[m0_edge_he_tgt];
                        const hd_t ps_he = ps.halfedge(ps_he_src, ps_he_tgt);

                        if (ps_he == hmesh_t::null_halfedge()) {
                            continue;
                        }

                        if (ps.face(ps_he) == ps_face) {
                            first_boundary_halfedge = m0_edge_he;
                            break;
                        }
                    } else { // x-->x OR o-->x OR x-->o

                        // o-->x : We want the ihalfedges which point into the sm whose tgt lays on the
                        // sm-face of tgt (they have an opposite direction wrt the face normal)
                        const bool is_ox = (!m0_edge_he_src_is_ivertex && m0_edge_he_tgt_is_ivertex);

                        if (is_ox) {
                            MCUT_ASSERT((size_t)m0_edge_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_edge_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                            const std::pair<ed_t, fd_t>& m0_edge_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_edge_he_tgt - ps_vtx_cnt);

                            // get the incident ps-halfedge of tgt
                            ed_t tgt_ps_e = m0_edge_he_tgt_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_edge_he_tgt);
                            hd_t tgt_ps_h = ps.halfedge(tgt_ps_e, 0);

                            if (ps.face(tgt_ps_h) != ps_face) {
                                tgt_ps_h = ps.opposite(tgt_ps_h); // i.e. "m0.halfedge(tgt_ps_e, 1);"
                                MCUT_ASSERT(tgt_ps_h != hmesh_t::null_halfedge()); // must be true if ps_face exists!
                            }

                            const vd_t& m0_edge_he_src_as_ps_vertex = m0_to_ps_vtx[m0_edge_he_src];

                            if (m0_edge_he_src_as_ps_vertex == ps.source(tgt_ps_h)) { // is counter clock-wise halfedge
                                first_boundary_halfedge = m0_edge_he;
                                break;
                            }
                        } else { // x-->x OR x-->o

                            const bool is_xx = m0_edge_he_src_is_ivertex && m0_edge_he_tgt_is_ivertex;

                            if (is_xx) { // exterior interior-iedge

                                // const hd_t src_coincident_ps_halfedge = m0_ivtx_to_ps_edge, m0_edge_he_src);
                                // const hd_t tgt_ps_h = m0_ivtx_to_ps_edge, m0_edge_he_tgt);
                                MCUT_ASSERT((size_t)m0_edge_he_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_edge_he_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                const std::pair<ed_t, fd_t>& m0_edge_he_src_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_edge_he_src - ps_vtx_cnt);
                                MCUT_ASSERT((size_t)m0_edge_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_edge_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                                const std::pair<ed_t, fd_t>& m0_edge_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_edge_he_tgt - ps_vtx_cnt);

                                const ed_t src_ps_edge = m0_edge_he_src_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_edge_he_src); // ps.edge(src_coincident_ps_halfedge)

                                const ed_t tgt_ps_edge = m0_edge_he_tgt_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_edge_he_tgt); // ps.edge(tgt_ps_h);
                                const bool is_exterior_ih = (src_ps_edge == tgt_ps_edge);

                                if (!is_exterior_ih) {
                                    continue; // interior ihalfedges cannot be used as the first exterior ihalfedge
                                }

                                /*
                                At this point, vertex information alone is insufficient to select the correct
                                ihalfedge from the edge. This is because the two vertices are topologically equivalent
                                if we just try to distinguish them following similar rules as other iedge types.
                                To solve this problem, we must instead use the connectivity information of
                                the polygon soup by relying on the incident ps-halfedge common to both vertices
                                (in their registry entries). The rough idea/steps:

                                1. get incident ps-halfedge incident to both src and tgt
                                2. get m0 edges incident to the edge on <1>
                                3. sort <2> with the first edge containing the source of <1>
                                4. get the halfedge sequence in <3> where src of the first he is the src of <1> and the tgt of the last he is the tgt of <1>
                                5. get the halfedge in <4> which is incident to the same ivertices as the current potential first-boundary halfedge
                                6. if the ps-face of <1> is the same as the current face
                                    7. set first polygon-boundary halfedge as <5>
                                8. else
                                    9. set first polygon-boundary halfedge as opposite of <5>
                            */

                                // 1. get incident ps-halfedge incident to both src and tgt
                                hd_t ps_halfedge_of_face = ps.halfedge(tgt_ps_edge, 0); // tgt_ps_h;

                                // equivalent to the check done at step 6. so that we know the correct halfedge to use in the steps ahead
                                if (ps.face(ps_halfedge_of_face) != ps_face) {
                                    ps_halfedge_of_face = ps.opposite(ps_halfedge_of_face);
                                    MCUT_ASSERT(ps_halfedge_of_face != hmesh_t::null_halfedge()); // guarranteed to exist since we have a poly-boundary interior ihalfedge
                                }

                                // 2. get m0 edges incident to the edge on <1>
                                const ed_t& incident_ps_edge = ps.edge(ps_halfedge_of_face);
                                std::map<ed_t, std::vector<ed_t>>::const_iterator ps_to_m0_edges_find_iter = ps_to_m0_edges.find(incident_ps_edge);
                                MCUT_ASSERT(ps_to_m0_edges_find_iter != ps_to_m0_edges.cend()); // because incident_ps_edge contains a polygon exterior interior ihalfedge

                                // 3. sort <2> with the first edge containing the source of <1>
                                //  NOTE: The edges are already sorted based on how we created "edges between the sorted vertices that are coincident on the same
                                //  ps-edge that has more-than 3 incident vertices." (Refer to that step for details)
                                const std::vector<ed_t>& sorted_m0_edges = ps_to_m0_edges_find_iter->second;

                                // 4. get the halfedge sequence in <3> where src of the first he is the src of <1> and the tgt of the last he is the tgt of <1>
                                std::vector<hd_t> halfedge_sequence;

                                // add the first halfedge (its source must be an original vertex)
                                // NOTE: its does not matter whether "first_he" point in the wrong direction or not right now (read on...)
                                const ed_t& first_e = sorted_m0_edges.front();
                                hd_t first_he = m0.halfedge(first_e, 0);
                                vd_t first_he_src = m0.source(first_he);

                                if (m0_is_intersection_point(first_he_src, ps_vtx_cnt)) {
                                    // TODO: I think this scope is never entered based on how we created "sorted_m0_edges".
                                    // Thus, the source vertex of the first halfedge in the sequence cannot be an intersection point
                                    first_he = m0.halfedge(first_e, 1);
                                    first_he_src = m0.source(first_he);
                                    MCUT_ASSERT(!m0_is_intersection_point(first_he_src, ps_vtx_cnt)); // expect original vertex since halfedge edge is the first in sequence
                                }

                                halfedge_sequence.push_back(first_he);

                                // get the remaining halfedge of sequence
                                for (std::vector<ed_t>::const_iterator seq_edge_iter = sorted_m0_edges.cbegin() + 1; // we have already added the first halfedge
                                     seq_edge_iter != sorted_m0_edges.cend(); ++seq_edge_iter) {

                                    // if (seq_edge_iter == sorted_m0_edges.cbegin()) {
                                    //     continue; // we have already added the first halfedge
                                    // }

                                    const ed_t& e = *seq_edge_iter;
                                    const hd_t h0 = m0.halfedge(e, 0);

                                    if (m0.source(h0) == m0.target(halfedge_sequence.back())) {
                                        halfedge_sequence.emplace_back(h0);
                                    } else {
                                        const hd_t h1 = m0.halfedge(e, 1);
                                        halfedge_sequence.emplace_back(h1);
                                    }
                                }

                                // we have our sequence but it is not gurranteed to point in the correct direction
                                MCUT_ASSERT(halfedge_sequence.size() == sorted_m0_edges.size());

                                const vd_t& first_he_src_as_ps_vertex = m0_to_ps_vtx[first_he_src]; // first he of sequence

                                if (first_he_src_as_ps_vertex != ps.source(ps_halfedge_of_face)) {

                                    // flip the sequence to make it point in the right direction
                                    std::for_each(
                                        halfedge_sequence.begin(),
                                        halfedge_sequence.end(),
                                        [&](hd_t& he) {
                                            he = m0.opposite(he);
                                        }); // flip seq
                                }

                                // 5. get the halfedge in <4> which is incident to the same ivertices as the current potential first-exterior halfedge

                                std::vector<hd_t>::const_iterator matching_he_find_iter = std::find_if(
                                    halfedge_sequence.cbegin(),
                                    halfedge_sequence.cend(),
                                    [&](const hd_t& he) {
                                        const vd_t& he_src = m0.source(he);
                                        const vd_t& he_tgt = m0.target(he);

                                        return (he_src == m0_edge_he_src && he_tgt == m0_edge_he_tgt /*|| (he_src == m0_edge_he_tgt && he_tgt == m0_edge_he_src*/);
                                    });

                                MCUT_ASSERT(matching_he_find_iter != halfedge_sequence.cend()); // does the potential halfedge actually point in the correct direction or not
                                {
                                    // 6. if the ps-face of <1> is the same as the current face
                                    //      7. set first polygon-exterior halfedge as <5>
                                    // 8. else
                                    //      9. set first polygon-exterior halfedge as opposite of <5>
                                    // if (ps.face(incident_ps_halfedge) == ps_face) {
                                    first_boundary_halfedge = *matching_he_find_iter;
                                    break;
                                }
                            } // if (is_xx) {
                            else {
                                // TODO: implement logic for is_xo

                                // NOTE: The code is able to work without implementing this scope due to the order-dependent nature in which
                                // edges are traversed (its guarranteed that oo ox and xx edges are encountered first) to find the first
                                // boundary halfedge
                            }
                        } // } else { // x-->x OR x-->o
                    } // } else { // x-->x OR o-->x OR x-->o
                } // for (int edge_he_iter = 0; edge_he_iter < 2; ++edge_he_iter) {

                if (first_boundary_halfedge != hmesh_t::null_halfedge()) {
                    break; // done
                }
            }

            MCUT_ASSERT(first_boundary_halfedge != hmesh_t::null_halfedge());

            // Now that we have a halfedge which lies on the boundary of the clipped polygon,
            // we will traverse/walk the clipped polygon's exterior to collect all other boundary halfedges
            // that have the same winding order as the input meshes (i.e. the source-mesh and cut-mesh).

            hd_t current_exterior_halfedge = hmesh_t::null_halfedge();
            hd_t next_exterior_halfedge = first_boundary_halfedge;
            std::unordered_map<ed_t, bool> walked_edges;
            // walked_edges.reserve(incident_edges.size());
            do {

                current_exterior_halfedge = next_exterior_halfedge;
                incident_halfedges.emplace_back(current_exterior_halfedge);
                walked_edges[m0.edge(current_exterior_halfedge)] = true;

                const vd_t current_tgt = m0.target(current_exterior_halfedge);
                next_exterior_halfedge = hmesh_t::null_halfedge(); // reset

                // find next boundary halfedge from incident edges
                for (std::vector<ed_t>::const_iterator incident_edge_iter = incident_edges.cbegin();
                     incident_edge_iter != incident_edges.cbegin() + incident_boundary_edge_count; // we only want exterior edges;
                     ++incident_edge_iter) {
                    // const int incident_edge_idx = (int)std::distance(incident_edges.cbegin(), incident_edge_iter);
                    // if (incident_edge_idx >= incident_boundary_edge_count) {
                    //     continue; // we only want exterior halfedge
                    // }

                    const ed_t& edge = *incident_edge_iter;

                    bool edge_walked = walked_edges.find(edge) != walked_edges.cend(); // std::find(walked_edges.cbegin(), walked_edges.cend(), edge) != walked_edges.cend();

                    if (edge_walked) {
                        continue; // skip edge is walked already
                    }

                    const vd_t v0 = m0.vertex(edge, 0);
                    const vd_t v1 = m0.vertex(edge, 1);

                    if (v0 == current_tgt || v1 == current_tgt) // check if connected to current (i.e. they share one vertex)
                    {
                        const bool v0_is_ivtx = m0_is_intersection_point(v0, ps_vtx_cnt);
                        const bool v1_is_ivtx = m0_is_intersection_point(v1, ps_vtx_cnt);
                        bool is_ambiguious_boundary_edge_case = v0_is_ivtx && v1_is_ivtx;
                        bool is_valid_ambiguious_boundary_edge = false;

                        if (is_ambiguious_boundary_edge_case) { // exterior edge with two intersection vertices (ambigious case arising from concave polyhedron cut)

                            MCUT_ASSERT((size_t)v0 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(v0) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                            const std::pair<ed_t, fd_t>& v0_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, v0 - ps_vtx_cnt);
                            const ed_t v0_ps_edge = v0_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, v0); //ps.edge(v0_coincident_ps_halfedge);

                            MCUT_ASSERT((size_t)v1 - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(v1) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                            const std::pair<ed_t, fd_t>& v1_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, v1 - ps_vtx_cnt);
                            const ed_t v1_ps_edge = v1_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, v1); // ps.edge(v1_coincident_ps_halfedge);

                            is_valid_ambiguious_boundary_edge = (v0_ps_edge == v1_ps_edge); // see also above when gathering exterior incident edges
                        }

                        if (!is_ambiguious_boundary_edge_case || is_valid_ambiguious_boundary_edge) {
                            const hd_t h0 = m0.halfedge(edge, 0);
                            const hd_t h1 = m0.halfedge(edge, 1);

                            next_exterior_halfedge = h0;

                            if (m0.source(h0) != current_tgt) // h0 is facing the opposite dir
                            {
                                next_exterior_halfedge = h1;
                            }

                            break; // found
                        }
                    }
                }

            } while (next_exterior_halfedge != hmesh_t::null_halfedge() /*first_boundary_halfedge*/);

            MCUT_ASSERT(incident_halfedges.size() >= 3); // minimum i.e. for a triangles!

            // Note: at this stage we have gathered all of the [exterior] halfedges needed to traced child polygons

            const int exterior_halfedge_count = (int)incident_halfedges.size();

            MCUT_ASSERT(exterior_halfedge_count == incident_boundary_edge_count);

            // Now we are going to also gather interior halfedges (those passing inside the area of "ps_face", and defined only by intersection points
            // where the src and tgt vertex do not share the same incident ihalfedge in their registry entry.

            for (std::vector<ed_t>::const_iterator incident_edge_iter = incident_edges.cbegin() + incident_boundary_edge_count; // start from polygon interior edges offset
                 incident_edge_iter != incident_edges.cend();
                 ++incident_edge_iter) {

                const ed_t& edge = (*incident_edge_iter);

                const hd_t h0 = m0.halfedge(edge, 0);
                const hd_t h1 = m0.halfedge(edge, 1);
                incident_halfedges.emplace_back(h0);
                incident_halfedges.emplace_back(h1);
            }

            // Note: at this stage, we have all the halfedges that we need to trace child polygons.
            // Thus, the next step is the actual tracing to clip ps_face

            // Trace child polygons on ps_face to clip it
            //-------------------------------------------

            std::vector<hd_t> incident_halfedges_to_be_walked(incident_halfedges.cbegin(), incident_halfedges.cend()); // copy

            do { // each iteration traces a child polygon

                traced_polygon_t child_polygon;

                hd_t current_halfedge = hmesh_t::null_halfedge();
                // can be any boundary halfedge in vector (NOTE: boundary halfedges come first in the std::vector)
                // Its important that we start from boundary halfedge as it simplies the conditions for when a
                // valid polygon has been constructed
                hd_t next_halfedge = incident_halfedges_to_be_walked[0];

                MCUT_ASSERT(incident_halfedges_to_be_walked.size() >= 2);

                bool is_valid_polygon = false;
                do { // each iteration walks a halfedge to incremetally trace a child polygon

                    // 1. update state
                    current_halfedge = next_halfedge;

                    child_polygon.push_back(current_halfedge);
                    const vd_t current_halfedge_target = m0.target(current_halfedge);
                    next_halfedge = hmesh_t::null_halfedge(); // reset

                    // remove next halfedge so that we dont walk it again
                    {
                        std::vector<hd_t>::iterator find_iter = std::find(incident_halfedges_to_be_walked.begin(), incident_halfedges_to_be_walked.end(), current_halfedge);
                        MCUT_ASSERT(find_iter != incident_halfedges_to_be_walked.end());
                        incident_halfedges_to_be_walked.erase(find_iter); // remove
                    }

                    if (child_polygon.size() >= 3) { // minimum halfedge count to constitute a valid polygon (triangle)

                        // source of first halfedge is target of last
                        if (m0.source(child_polygon.front()) == m0.target(child_polygon.back())) {

                            // the current halfedge is [not] the opposite of the first halfedge in list
                            // This is an important edge case for when you walk a halfedge connecting two vertices not in alpha (intersection).
                            // Example: case of tracing a polygon analogous to a cheek slash.
                            // See also the comment above the declaration of "next_halfedge"
                            if (current_halfedge != m0.opposite(child_polygon.front())) {
                                is_valid_polygon = true;

                                break;
                            } else { // ... if the current halfedge is the opposite of the first halfedge in list

                                // peak forward to see what the next halfedge will be (if the next halfedge is in "child_polygon" then we are done)
                                std::vector<hd_t> premptive_candidate_halfedges;
                                for (std::vector<hd_t>::const_iterator incident_halfedges_to_be_walked_iter = incident_halfedges_to_be_walked.cbegin(); incident_halfedges_to_be_walked_iter != incident_halfedges_to_be_walked.cend(); ++incident_halfedges_to_be_walked_iter) {
                                    const hd_t& potential_candidate = *incident_halfedges_to_be_walked_iter;
                                    if (m0.source(potential_candidate) == current_halfedge_target) {
                                        premptive_candidate_halfedges.push_back(potential_candidate);
                                    }
                                }

                                hd_t prime_candidate = hmesh_t::null_halfedge();
                                if (premptive_candidate_halfedges.size() == 2) {

                                    const std::vector<hd_t>::const_iterator current_halfedge_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                                    MCUT_ASSERT(current_halfedge_find_iter != incident_halfedges.cend());
                                    const bool current_halfedge_is_exterior = std::distance(incident_halfedges.cbegin(), current_halfedge_find_iter) < exterior_halfedge_count;

                                    if (current_halfedge_is_exterior) {

                                        // pick interior candidate
                                        const std::vector<hd_t>::const_iterator prime_candidate_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                                        MCUT_ASSERT(prime_candidate_find_iter != incident_halfedges.cend());
                                        const bool prime_candidate_is_exterior = std::distance(incident_halfedges.cbegin(), prime_candidate_find_iter) < exterior_halfedge_count;

                                        if (prime_candidate_is_exterior) {
                                            prime_candidate = premptive_candidate_halfedges.back(); // select correct interior halfedge
                                        }
                                    } else { // interior

                                        // pick non-opposite
                                        const bool prime_candidate_is_opposite = m0.opposite(current_halfedge) == prime_candidate;

                                        if (prime_candidate_is_opposite) {
                                            prime_candidate = premptive_candidate_halfedges.back(); // select correct non-opposite halfedge
                                        }
                                    }
                                }

                                const hd_t premptive_next = prime_candidate;

                                MCUT_ASSERT(premptive_candidate_halfedges.size() <= 2);

                                if (std::find(child_polygon.cbegin(), child_polygon.cend(), premptive_next) != child_polygon.cend()) {
                                    is_valid_polygon = true;

                                    break;
                                }
                            }
                        }
                    }

                    // 2. find next halfedge

                    // 2.1. get candidates (halfedges whose source vertex is the target of current)
                    std::vector<hd_t> candidate_halfedges;
                    candidate_halfedges.reserve(2); // two candidates at most because we filtered out exterior interior-halfedges as well as clockwise (cw) halfedge

                    for (std::vector<hd_t>::const_iterator incident_halfedges_to_be_walked_iter = incident_halfedges_to_be_walked.cbegin(); incident_halfedges_to_be_walked_iter != incident_halfedges_to_be_walked.cend(); ++incident_halfedges_to_be_walked_iter) {
                        const hd_t& potential_candidate = *incident_halfedges_to_be_walked_iter;
                        if (m0.source(potential_candidate) == current_halfedge_target) {
                            candidate_halfedges.push_back(potential_candidate);
                        }
                    }

                    MCUT_ASSERT(candidate_halfedges.size() <= 2);

                    // 2.2. select prime candidate
                    hd_t prime_candidate = hmesh_t::null_halfedge();

                    if (!candidate_halfedges.empty()) {
                        prime_candidate = candidate_halfedges[0]; // assuming: candidate_halfedges.size() == 1
                    }

                    if (candidate_halfedges.size() == 2) {

                        const std::vector<hd_t>::const_iterator current_halfedge_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                        MCUT_ASSERT(current_halfedge_find_iter != incident_halfedges.cend());
                        const bool current_halfedge_is_exterior = std::distance(incident_halfedges.cbegin(), current_halfedge_find_iter) < exterior_halfedge_count;

                        if (current_halfedge_is_exterior) {

                            // pick interior candidate
                            const std::vector<hd_t>::const_iterator prime_candidate_find_iter = std::find(incident_halfedges.cbegin(), incident_halfedges.cend(), current_halfedge);
                            MCUT_ASSERT(prime_candidate_find_iter != incident_halfedges.cend());
                            const bool prime_candidate_is_exterior = std::distance(incident_halfedges.cbegin(), prime_candidate_find_iter) < exterior_halfedge_count;

                            if (prime_candidate_is_exterior) {
                                prime_candidate = candidate_halfedges.back(); // select correct interior halfedge
                            }
                        } else { // interior

                            // pick non-opposite
                            const bool prime_candidate_is_opposite = m0.opposite(current_halfedge) == prime_candidate;

                            if (prime_candidate_is_opposite) {
                                prime_candidate = candidate_halfedges.back(); // select correct non-opposite halfedge
                            }
                        }
                    }

                    next_halfedge = prime_candidate;

                } while (next_halfedge != hmesh_t::null_halfedge());

                if (is_valid_polygon) {

                    const int poly_idx = (int)(m0_polygons.size() + child_polygons.size());
                    if (ps_is_cutmesh_face(ps_face, sm_face_count)) {
                        m0_cm_cutpath_adjacent_polygons.push_back(poly_idx);
                    } else {
                        m0_sm_cutpath_adjacent_polygons.push_back(poly_idx);
                    }

                    m0_to_ps_face[poly_idx] = ps_face;

                    child_polygons.emplace_back(child_polygon);
                }

            } while (!incident_halfedges_to_be_walked.empty());
        } // if (!is_intersecting_ps_face) {

        m0_polygons.insert(m0_polygons.end(), child_polygons.cbegin(), child_polygons.cend());

        if (!is_from_cut_mesh /*!ps_is_cutmesh_face(ps_face, sm_face_count)*/) {
            traced_sm_polygon_count += (int)child_polygons.size();
        }

    } // for each ps-face to trace

#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

    TIMESTACK_POP(); // &&&&&

    // m0_ivtx_to_ps_faces.clear(); // free
    ps_iface_to_m0_edge_list.clear(); // free
    ps_to_m0_edges.clear(); // free
    ps_to_m0_non_intersecting_edge.clear(); // free
    ps_iface_to_ivtx_list.clear(); // free

    // Note: at this stage, we have traced all polygons. This means that any intersecting face in the polygon
    // soup data structure will also now have been clipped.
    //
    // The connectivity of all traced polygons is stored as a vector/array of halfedges, for each
    // traced polygon. The halfedge data structure (i.e. "m0") still holds the underlying mesh data
    // over-which we are abstracting this connectivity i.e. "m0" stores vertices (e.g. intersection
    // points), edges, and halfeges.
    //
    // The lists of halfedges that we are using to represent the traced polygons avoids "2-manifold restrictions":
    // Storing the traced polygons inside a halfedge data structure is not always possible because we could violate the
    // priniciple rule that an edge must be incident to at-most 2 faces (2-manifold surface mesh rule).
    //
    // There is a other benefit to using lists: it makes for a more logical implementation for the remainder of the
    // cutting algorithm i.e when duplicating intersection points, creating cut-mesh patches, stitching (hole
    // filling), and more.

    MCUT_ASSERT((int)m0_polygons.size() >= ps.number_of_faces());

    const std::vector<traced_polygon_t>::iterator traced_sm_polygons_iter_end = m0_polygons.begin() + traced_sm_polygon_count;
    // const std::vector<traced_polygon_t>::iterator& traced_cs_polygons_iter_begin = traced_sm_polygons_iter_end;
    const std::vector<traced_polygon_t>::const_iterator m0_traced_sm_polygons_iter_cend = m0_polygons.cbegin() + traced_sm_polygon_count;
    const std::vector<traced_polygon_t>::const_iterator& traced_cs_polygons_iter_cbegin = traced_sm_polygons_iter_end;

    TIMESTACK_PUSH("Mark seam edges *");
    // extract the seam vertices
    std::vector<bool> m0_vertex_to_seam_flag;
    mark_seam_vertices(m0_vertex_to_seam_flag, m0, ps_vtx_cnt);
    TIMESTACK_POP();

    MCUT_ASSERT(!m0_vertex_to_seam_flag.empty());

    ///////////////////////////////////////////////////////////////////////////
    // Dump meshes for the source-mesh and cut-mesh using the traced polygons
    ///////////////////////////////////////////////////////////////////////////

    //
    // NOTE: we cannot always create meshes using the traced polygons because of
    // a possible violation of the surface mesh contruction rules. Basically, we cannot
    // reference a halfedge and its opposite in the same face because it violates
    // halfedge construction rules (2-manifold surface mesh). This issue occurs
    // whenever ps polygon is partially cut.
    //
    // Thus, we will only dump meshes if can gaurranteed not to violate halfedge
    // mesh rules (to avoid potentially crashing the program due to logic error).
    //

    bool all_cutpaths_are_circular = (num_explicit_circular_cutpaths == num_explicit_cutpath_sequences);

    // dump traced src-mesh polygons.
    if (input.keep_srcmesh_seam) {
        // dump traced polygons only if the cut paths are circular or complete linear cuts (prevents us
        // from violating halfedge construction rules)

        bool all_cutpaths_linear_and_without_making_holes = (num_explicit_circular_cutpaths == 0) && ((int)explicit_cutpaths_severing_srcmesh.size() == num_explicit_linear_cutpaths);

        if (cm_is_watertight || (all_cutpaths_are_circular || all_cutpaths_linear_and_without_making_holes)) {

            std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>> separated_src_mesh_fragments;
            std::unordered_map<int, int> _1;
            // NOTE: The result is a mesh identical to the original source mesh except at the edges introduced by the cut..
            extract_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                *input.scheduler,
#endif
                separated_src_mesh_fragments,
                m0,
                0, // no offset because traced source-mesh polygons start from the beginning of "m0_polygons"
                std::vector<traced_polygon_t>(m0_polygons.begin(), traced_sm_polygons_iter_end),
                std::vector<int>(), // sm_polygons_below_cs
                std::vector<int>(), // sm_polygons_above_cs
                m0_vertex_to_seam_flag,
                std::vector<vd_t>(), // Unused ... because we are extracting from "m0"
                std::unordered_map<vd_t, vd_t>(), // Unused ... because we are extracting from "m0"
                _1, // Unused ... because we are extracting from "m0"
                m0_to_ps_vtx,
                m0_to_ps_face,
                ps_to_sm_vtx,
                ps_to_sm_face,
                ps_to_cm_vtx,
                ps_to_cm_face,
                sm_vtx_cnt,
                sm_face_count,
                input.populate_vertex_maps,
                input.populate_face_maps,
                false, // unused ...
                false, // unused ...
                false // unused ...
            );

            MCUT_ASSERT(separated_src_mesh_fragments.size() == 1); // one cc
            MCUT_ASSERT(separated_src_mesh_fragments.cbegin()->second.size() == 1); // one instance
            output.seamed_src_mesh = std::shared_ptr<output_mesh_info_t>(new output_mesh_info_t);
            output.seamed_src_mesh->mesh = (separated_src_mesh_fragments.begin()->second.front().first);
            output.seamed_src_mesh->seam_vertices = std::move(separated_src_mesh_fragments.begin()->second.front().second.seam_vertices);
            output.seamed_src_mesh->data_maps = std::move(separated_src_mesh_fragments.begin()->second.front().second.data_maps);

            if (input.verbose) {
                dump_mesh(output.seamed_src_mesh->mesh.get()[0], "src-mesh-traced-poly");
            }
        }
    } // if (input.include_seam_srcmesh) {

    // dump traced cut-mesh polygons
    if (input.keep_cutmesh_seam) {

        // bool all_cutpaths_linear_and_make_holes = (num_explicit_circular_cutpaths == 0) && (explicit_cutpaths_severing_srcmesh.size() == 0);

        if (sm_is_watertight || (all_cutpaths_are_circular /*|| all_cutpaths_linear_and_make_holes*/)) {
            std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>> separated_cut_mesh_fragments;
            std::unordered_map<int, int> _1;
            hmesh_t merged = extract_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                *input.scheduler,
#endif
                separated_cut_mesh_fragments,
                m0,
                traced_sm_polygon_count, // offset to start of traced cut-mesh polygons in "m0_polygons".
                std::vector<traced_polygon_t>(traced_cs_polygons_iter_cbegin, m0_polygons.cend()),
                std::vector<int>(),
                std::vector<int>(),
                m0_vertex_to_seam_flag,
                std::vector<vd_t>(), // Unused ... because we are extracting from "m0"
                std::unordered_map<vd_t, vd_t>(), // Unused ... because we are extracting from "m0"
                _1, // Unused ... because we are extracting from "m0"
                m0_to_ps_vtx,
                m0_to_ps_face,
                ps_to_sm_vtx,
                ps_to_sm_face,
                ps_to_cm_vtx,
                ps_to_cm_face,
                sm_vtx_cnt,
                sm_face_count,
                input.populate_vertex_maps,
                input.populate_face_maps,
                false, // Unused ...
                false, // Unused ...
                false // Unused ...
            );

            if (separated_cut_mesh_fragments.size() == 1) { // usual case
                MCUT_ASSERT(separated_cut_mesh_fragments.cbegin()->second.size() == 1); // one instance
                output.seamed_cut_mesh = std::shared_ptr<output_mesh_info_t>(new output_mesh_info_t);
                output.seamed_cut_mesh->mesh = (separated_cut_mesh_fragments.begin()->second.front().first);
                output.seamed_cut_mesh->seam_vertices = std::move(separated_cut_mesh_fragments.begin()->second.front().second.seam_vertices);
                output.seamed_cut_mesh->data_maps = std::move(separated_cut_mesh_fragments.begin()->second.front().second.data_maps);

                if (input.verbose) {
                    dump_mesh(output.seamed_cut_mesh->mesh.get()[0], "cut-mesh-traced-poly");
                }
            }
        }
    } // if (input.include_seam_srcmesh) {

    if (false == (input.keep_fragments_below_cutmesh || //
            input.keep_fragments_above_cutmesh || //
            input.keep_fragments_partially_cut || //
            input.keep_unsealed_fragments || //
            input.keep_fragments_sealed_inside || //
            input.keep_fragments_sealed_outside || input.keep_fragments_sealed_inside_exhaustive || //
            input.keep_fragments_sealed_outside_exhaustive || //
            input.keep_inside_patches || //
            input.keep_outside_patches)) {
        // if the user simply wants seams, then we should not have to proceed further.
        return;
    }

    ///////////////////////////////////////////////////////////////////////////
    // Map each halfedge to the traced polygons that uses it
    ///////////////////////////////////////////////////////////////////////////

    // We now need to manually maintain halfedge incidence (i.e. "used-by") information since
    // the traced-polygon connectivity is not stored inside our halfedge mesh data structure.
    // A halfedge data structure would normally store such incidence information for us but this is
    // no longer possible for reasons mentioned above (see long comment after tracing loop).
    //
    // So the first incidence information that we need to keep around is the mapping from every
    // halfedge (in "m0") which is used to trace a polygon, to the traced polygon(s) that uses
    // that halfedge. Thus, halfedges which are not used for tracing [at all] have an entry in this
    // vector but the value (std::vector) is empty. We will use this information later, like to
    // stitch cut-mesh patches to src-mesh fragments.

    TIMESTACK_PUSH("Map halfedges to polygons");
    // std::map<
    //     hd_t,            // a halfedge that is used to trace a polygon
    //    std::vector<int> // list of indices of  traced polygons that are traced with the halfedge
    //    >
    //    m0_h_to_ply;
    std::vector<std::vector<int>> m0_h_to_ply(m0.number_of_halfedges());

    // for each traced polygon
    for (std::vector<traced_polygon_t>::const_iterator traced_polygon_iter = m0_polygons.cbegin();
         traced_polygon_iter != m0_polygons.cend();
         ++traced_polygon_iter) {

        const traced_polygon_t& traced_polygon = *traced_polygon_iter;
        const int traced_polygon_index = (int)std::distance(m0_polygons.cbegin(), traced_polygon_iter);

        // for each halfedge in polygon
        for (traced_polygon_t::const_iterator traced_polygon_halfedge_iter = traced_polygon.cbegin();
             traced_polygon_halfedge_iter != traced_polygon.cend();
             ++traced_polygon_halfedge_iter) {

            const hd_t& traced_polygon_halfedge = *traced_polygon_halfedge_iter;
#if 0
                

                std::pair<std::map<hd_t, std::vector<int>>::iterator, bool> pair = m0_h_to_ply.insert(std::make_pair(traced_polygon_halfedge, std::vector<int>()));

                if (pair.second == false) // element exists (m0 halfedges (only interior ihalfedges) can be reused by more than one polygon. upto two polygons!)
                {
                    MCUT_ASSERT(!pair.first->second.empty());
                    MCUT_ASSERT(std::find(pair.first->second.cbegin(), pair.first->second.cend(), traced_polygon_index) == pair.first->second.cend());
                }

                pair.first->second.push_back(traced_polygon_index);
                MCUT_ASSERT(pair.first->second.size() <= 2);
#else
            SAFE_ACCESS(m0_h_to_ply, traced_polygon_halfedge).push_back(traced_polygon_index);
            MCUT_ASSERT(SAFE_ACCESS(m0_h_to_ply, traced_polygon_halfedge).size() <= 2);
#endif
        }
    }

#if 0
    // dump
    for (std::map<hd_t, std::vector<int>>::const_iterator i = m0_h_to_ply.cbegin(); i != m0_h_to_ply.cend(); ++i) {
        
        for (std::vector<int>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) {
            
            
            
        }
        
    }
#endif

    TIMESTACK_POP();

    // bool all_cutpaths_make_holes = ((int)explicit_cutpaths_making_holes.size() == num_explicit_cutpath_sequences);

    TIMESTACK_PUSH("Find exterior cut-mesh polygons");
    ///////////////////////////////////////////////////////////////////////////
    // Find all cut-mesh polygons which are "exterior" relative to the source-mesh
    ///////////////////////////////////////////////////////////////////////////

    // Here we will explicitly find a subset of the traced cut-mesh polygons which lie
    // outside/exterior w.r.t the source-mesh. We find these polygons using the
    // "re-entrant" vertices that where identified while calculating intersection
    // points. These will be used (later) to mark cut-mesh patches as either interior
    // or exterior w.r.t the source-mesh.
    //
    // Note that these polygons will be the new "child polygons" which are new as a result of
    // the intersections.

    // An element here represents the index of an exterior cut-mesh polygon, and the index of
    // halfedge which touches the source-mesh and points torward the interior (inside) of the src-mesh.
    std::unordered_map<int /*m0 cs poly*/, int /*he idx*/> known_exterior_cm_polygons;

    if (explicit_cutpaths_making_holes.size() > 0) { // atleast one cut-path makes a hole to be sealed later

        // for each traced cut-mesh polygon

        for (std::vector<int>::const_iterator cs_ipoly_iter = m0_cm_cutpath_adjacent_polygons.cbegin();
             cs_ipoly_iter != m0_cm_cutpath_adjacent_polygons.cend();
             ++cs_ipoly_iter) {
            // for (std::vector<traced_polygon_t>::const_iterator cs_poly_iter = traced_cs_polygons_iter_cbegin;
            //      cs_poly_iter != m0_polygons.cend();
            //      ++cs_poly_iter) {

            // const traced_polygon_t& cs_poly = *cs_poly_iter;
            // const int cs_poly_idx = (int)std::distance(m0_polygons.cbegin(), cs_poly_iter);
            const int cs_poly_idx = *cs_ipoly_iter;
            MCUT_ASSERT(cs_poly_idx < (int)m0_polygons.size());

            if (known_exterior_cm_polygons.find(cs_poly_idx) != known_exterior_cm_polygons.cend()) {
                continue; // we have already tagged the polygon as being exterior!
            }

            const traced_polygon_t& cs_poly = SAFE_ACCESS(m0_polygons, cs_poly_idx);

            // for each halfedge of polygon
            for (traced_polygon_t::const_iterator cs_poly_he_iter = cs_poly.cbegin();
                 cs_poly_he_iter != cs_poly.cend();
                 ++cs_poly_he_iter) {

                // we want to use class-1 ihalfedges : o-->x. This type of halfedge was the
                // one used to calculate re-entrant vertices
                const hd_t& cs_poly_he = *cs_poly_he_iter;
                const vd_t cs_poly_he_src = m0.source(cs_poly_he);
                const vd_t cs_poly_he_tgt = m0.target(cs_poly_he);
                const bool tgt_is_ivertex = m0_is_intersection_point(cs_poly_he_tgt, ps_vtx_cnt);
                const bool src_is_ivertex = m0_is_intersection_point(cs_poly_he_src, ps_vtx_cnt);

                if (!tgt_is_ivertex) {
                    continue; // either class-0 or class-2
                }

                // check that the target vertex is along a cut-path making a hole
                const int tgt_explicit_cutpath_sequence_idx = SAFE_ACCESS(m0_ivtx_to_cutpath_sequence, cs_poly_he_tgt);
                bool cutpath_makes_a_hole = std::find(explicit_cutpaths_making_holes.cbegin(),
                                                explicit_cutpaths_making_holes.cend(),
                                                tgt_explicit_cutpath_sequence_idx)
                    != explicit_cutpaths_making_holes.cend();

                if (cutpath_makes_a_hole == false) {
                    // skip because the patch of the curent polygon will not be used
                    // for sealing/stitching holes. Thus, there is no need to tag the
                    // polygon as being either interior or exterior. That is, its
                    // adjacent cutpath does not make a hole!
                    continue;
                }

                // get the intersection info which was calculated earlier (src-mesh normal vector )
                // NOTE: this is exactly the same numerical calculation that was computed previously.
                // const std::map<vd_t, vec3>::const_iterator cs_nonborder_reentrant_ivertices_find_iter = cm_nonborder_reentrant_ivtx_list.find(cs_poly_he_tgt);
                // const bool tgt_is_nonborder_reentrant_vertex = cs_nonborder_reentrant_ivertices_find_iter != cm_nonborder_reentrant_ivtx_list.cend();
                // std::vector<vd_t>::const_iterator border_reentrant_vertex_find_iter = std::find(cm_border_reentrant_ivtx_list.cbegin(), cm_border_reentrant_ivtx_list.cend(), cs_poly_he_tgt);
                // const bool tgt_is_border_reentrant_vertex = border_reentrant_vertex_find_iter != cm_border_reentrant_ivtx_list.cend();

                // MCUT_ASSERT(!(tgt_is_nonborder_reentrant_vertex && tgt_is_border_reentrant_vertex)); // a re-entrant vertex cannot be both a border and norborder

                // if (!tgt_is_nonborder_reentrant_vertex && !tgt_is_border_reentrant_vertex) {
                //     continue; // cs_poly_he_tgt is an ivertex but it is not a nonborder re-entrant vertex ( was not saved as one)
                // }

                // o-->x : We want the intersection halfedges which point "into" the source-mesh, i.e. whose tgt is on
                // the source-mesh face of tgt (found in the registry entry). This implies that the current
                // cut-mesh halfedge must have an opposite direction w.r.t the normal of the src-mesh face.
                const bool is_ox = (!src_is_ivertex && tgt_is_ivertex);
                bool is_boundary_ih = false; // i.e. is and intersecting halfedge

                MCUT_ASSERT((size_t)cs_poly_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(cs_poly_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                const std::pair<ed_t, fd_t>& cs_poly_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)cs_poly_he_tgt - ps_vtx_cnt);

                if (src_is_ivertex && tgt_is_ivertex) {
                    // const hd_t src_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(cs_poly_he));
                    //  const hd_t tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.target(cs_poly_he));
                    // const ed_t src_ps_edge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(cs_poly_he)); // ps.edge(src_coincident_ps_halfedge);
                    // const ed_t tgt_ps_edge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.target(cs_poly_he)); // ps.edge(tgt_ps_h);

                    MCUT_ASSERT((size_t)cs_poly_he_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /* m0_ivtx_to_intersection_registry_entry.find(cs_poly_he_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                    const std::pair<ed_t, fd_t>& cs_poly_he_src_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)cs_poly_he_src - ps_vtx_cnt);

                    is_boundary_ih = (cs_poly_he_src_ipair.first == cs_poly_he_tgt_ipair.first);
                }

                if (!(is_ox || is_boundary_ih)) {
                    continue; // is interior-ihalfedge
                }

                // if tgt is a border re-entrant vertex then the polygon is (by definition) on the border
                // given the properties of a border re-entrant vertex. Basically, this is due to the
                // type of halfedge that we are looking for and the fact that border re-entrant vertices
                // occur only on the border of the cut-mesh.
                // bool is_border_polygon = tgt_is_border_reentrant_vertex;

                // if (!is_border_polygon && tgt_is_nonborder_reentrant_vertex) {
                //  Re-calculate the geometry operation as as we did before we calculated the
                //  tgt-ivertex (i.e. with scalar product) using the halfedge's src and tgt
                //  coordinates and the normal of the face which was intersected to produce
                //  the tgt vertex.
                // const vec3& polygon_normal = cs_nonborder_reentrant_ivertices_find_iter->second;
                // MCUT_ASSERT(m0_ivtx_to_tested_polygon_normal.find(cs_poly_he_tgt) != m0_ivtx_to_tested_polygon_normal.cend());

                // get the registry entry edge

                // const std::vector<fd_t> set_edge0_v0_registry = ps_get_ivtx_registry_entry_faces(ps, set_edge0_v0_ipair);
                // const ed_t& registry_entry_edge = cs_poly_he_tgt_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, cs_poly_he_tgt);
                //  get registry entry faces

                // const std::vector<fd_t>& registry_entry_faces = SAFE_ACCESS(m0_ivtx_to_ps_faces, cs_poly_he_tgt);
                const std::vector<fd_t> registry_entry_faces = ps_get_ivtx_registry_entry_faces(ps, cs_poly_he_tgt_ipair);

                // get the registry-entry face which is not incident to edge
                const vd_t registry_entry_edge_v0 = ps.vertex(cs_poly_he_tgt_ipair.first, 0);
                std::vector<fd_t>::const_iterator tested_face; // which was intersected by "registry_entry_edge" to get "cs_poly_he_tgt"
                bool registry_entry_edge_is_from_cutmesh = ps_is_cutmesh_vertex(registry_entry_edge_v0, sm_vtx_cnt);
                if (registry_entry_edge_is_from_cutmesh) {
                    // ... then intersected face was from the source mesh
                    tested_face = std::find_if(registry_entry_faces.cbegin(), registry_entry_faces.cend(),
                        [&](const fd_t& f) {
                            bool is_sm_face = !ps_is_cutmesh_face(f, sm_face_count) && f != hmesh_t::null_face();
                            return is_sm_face;
                        });
                } else {
                    // ... then intersected face was from the cut mesh
                    tested_face = std::find_if(registry_entry_faces.cbegin(), registry_entry_faces.cend(),
                        [&](const fd_t& f) {
                            bool is_cm_face = ps_is_cutmesh_face(f, sm_face_count) && f != hmesh_t::null_face();
                            return is_cm_face;
                        });
                }

                MCUT_ASSERT(tested_face != registry_entry_faces.cend()); // "registry_entry_faces" must have at least one face from cm and at least one from sm
                MCUT_ASSERT(ps_tested_face_to_plane_normal.find(*tested_face) != ps_tested_face_to_plane_normal.cend());

                // get normal of face
                const vec3& polygon_normal = SAFE_ACCESS(ps_tested_face_to_plane_normal, *tested_face); // SAFE_ACCESS(m0_ivtx_to_tested_polygon_normal, cs_poly_he_tgt);
                // const vec3& polygon_normal = geometric_data.first; // source-mesh face normal
                // const double& orig_scalar_prod = geometric_data.second; // the dot product result we computed earlier

                // MCUT_ASSERT(sign(orig_scalar_prod) == NEGATIVE);

                // calculate the vector represented by the current halfedge
                const vec3 cs_poly_he_vector = m0.vertex(cs_poly_he_tgt) - m0.vertex(cs_poly_he_src);
                // calculate dot product with the src-mesh normal
                const double scalar_prod = dot_product(polygon_normal, cs_poly_he_vector);
                // the original ps-halfedge was "incoming" (pointing inwards) and gave a
                // negative scalar-product with the src-mesh face normal.
                // check that it is the same
                // Note: we want the same sign (i.e. cs_poly_he_vector has negative scalar-product)
                // because we want the class-1 ihalfedge which is exterior but points inside the src-mesh
                // is_border_polygon = (sign(scalar_prod) == NEGATIVE);
                //}

                if (sign(scalar_prod) == NEGATIVE) { // the current halfedge passed the sign test
                    MCUT_ASSERT(known_exterior_cm_polygons.find(cs_poly_idx) == known_exterior_cm_polygons.cend());
                    known_exterior_cm_polygons[cs_poly_idx] = (int)std::distance(cs_poly.cbegin(), cs_poly_he_iter);
                    break; // done, we now know "cs_poly_idx" as an exterior polygon
                }
            }
        }
    }

    // cm_nonborder_reentrant_ivtx_list.clear(); // free
    m0_ivtx_to_cutpath_sequence.clear(); // free

    TIMESTACK_POP();

    TIMESTACK_PUSH("Find source mesh polygon above and below cm");
    ///////////////////////////////////////////////////////////////////////////
    // Find the source-mesh polygons (next to cutpath) which are above and below
    ///////////////////////////////////////////////////////////////////////////

    //
    // We are searching through all of the traced source-mesh polygons to find those
    // which are adjacent to the cut path. We then identify them as being either
    // "above" or "below" the cut-mesh which we do using the source-mesh re-entrant
    // vertices.
    //

    std::vector<int> sm_polygons_below_cs;
    std::vector<int> sm_polygons_above_cs;

    // for each traced source-mesh polygon along cutpath
    for (std::vector<int>::const_iterator sm_ipoly_iter = m0_sm_cutpath_adjacent_polygons.cbegin();
         sm_ipoly_iter != m0_sm_cutpath_adjacent_polygons.cend();
         ++sm_ipoly_iter) {
        // for (std::vector<traced_polygon_t>::const_iterator sm_poly_iter = m0_polygons.cbegin();
        //    sm_poly_iter != traced_sm_polygons_iter_end;
        //  ++sm_poly_iter) {
        // const traced_polygon_t& sm_poly = *sm_poly_iter;
        // const int sm_poly_idx = (int)std::distance(m0_polygons.cbegin(), sm_poly_iter);
        const int sm_poly_idx = *sm_ipoly_iter;
        MCUT_ASSERT(sm_poly_idx < (int)m0_polygons.size());
        const traced_polygon_t& sm_poly = SAFE_ACCESS(m0_polygons, sm_poly_idx);

        // for each halfedge of polygon
        for (traced_polygon_t::const_iterator sm_poly_he_iter = sm_poly.cbegin();
             sm_poly_he_iter != sm_poly.cend();
             ++sm_poly_he_iter) {

            const hd_t& sm_poly_he = *sm_poly_he_iter; // we want class-1 ihalfedges : o-->x
            const vd_t sm_poly_he_src = m0.source(sm_poly_he);
            const vd_t sm_poly_he_tgt = m0.target(sm_poly_he);
            const bool tgt_is_ivertex = m0_is_intersection_point(sm_poly_he_tgt, ps_vtx_cnt);
            const bool src_is_ivertex = m0_is_intersection_point(sm_poly_he_src, ps_vtx_cnt);

            if (!tgt_is_ivertex) {
                continue; // either class-0 (o-->o) or class-2 (x-->o)
            }

            // const std::map<vd_t, vec3>::const_iterator sm_nonborder_reentrant_ivertices_find_iter = sm_nonborder_reentrant_ivtx_list.find(sm_poly_he_tgt);
            // const bool tgt_is_sm_nonborder_reentrant_vertex = sm_nonborder_reentrant_ivertices_find_iter != sm_nonborder_reentrant_ivtx_list.cend();

            // NOTE: we do not need source-mesh border re-entrant vertices because they are not useful for the
            // determining whether traced source-mesh polygons are either "above" or "below" the cut-mesh. The notion
            // of above or below is defined only for source-mesh fragements w.r.t. the cut-mesh. The is because
            // we are only interested in partitioning the source-mesh and not the cut-mesh
            //

            // if (!tgt_is_sm_nonborder_reentrant_vertex) {
            //    continue; // cs_poly_he_tgt is an ivertex but it is not a regular re-entrant vertex
            // }

            // o-->x : We want the intersection halfedges which point into the cut-mesh and whose tgt lays on the cut-mesh face of tgt
            // (they have an opposite direction wrt the face normal)
            const bool is_ox = (!src_is_ivertex && tgt_is_ivertex);

            const bool is_boundary_halfedge = m0_is_polygon_boundary_halfedge(sm_poly_he, m0_num_cutpath_halfedges);
            bool is_boundary_ih = is_boundary_halfedge;

            // const hd_t src_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, sm_poly_he_src);
            // const hd_t tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, sm_poly_he_tgt);

            // const ed_t tgt_ps_edge = SAFE_ACCESS(m0_ivtx_to_ps_edge, sm_poly_he_tgt); //ps.edge(tgt_ps_h);
            MCUT_ASSERT((size_t)sm_poly_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(sm_poly_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
            const std::pair<ed_t, fd_t>& sm_poly_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)sm_poly_he_tgt - ps_vtx_cnt);
            const ed_t& tgt_ps_edge = sm_poly_he_tgt_ipair.first;
#if 0
                if (src_is_ivertex && tgt_is_ivertex)
                {
                    MCUT_ASSERT((size_t)sm_poly_he_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(sm_poly_he_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                    const std::pair<ed_t, fd_t> &sm_poly_he_src_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, sm_poly_he_src - ps_vtx_cnt);
                    const ed_t &src_ps_edge = sm_poly_he_src_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, sm_poly_he_src); //ps.edge(src_coincident_ps_halfedge);

                    is_boundary_ih = (src_ps_edge == tgt_ps_edge);
                }
#endif
            if (!(is_ox || is_boundary_ih)) {
                continue;
            }

            // Re-calculate the exact same geometry operation as the time we calculated
            // the tgt-ivertex (scalar product using the halfedge's src and tgt coordinates
            // and and the normal of the cut-mesh face that was intersected to produce
            // the tgt vertex).
            // MCUT_ASSERT(m0_ivtx_to_tested_polygon_normal.find(sm_poly_he_tgt) != m0_ivtx_to_tested_polygon_normal.cend());
            // get the registry entry edge

            const ed_t& registry_entry_edge = tgt_ps_edge; // SAFE_ACCESS(m0_ivtx_to_ps_edge, sm_poly_he_tgt);
            // get registry entry faces
            const std::vector<fd_t> registry_entry_faces = ps_get_ivtx_registry_entry_faces(ps, sm_poly_he_tgt_ipair); // SAFE_ACCESS(m0_ivtx_to_ps_faces, sm_poly_he_tgt);
            // get the registry-entry face which is not incident to edge
            const vd_t registry_entry_edge_v0 = ps.vertex(registry_entry_edge, 0);
            std::vector<fd_t>::const_iterator tested_face; // which was intersected by "registry_entry_edge" to get "cs_poly_he_tgt"
            bool registry_entry_edge_is_from_cutmesh = ps_is_cutmesh_vertex(registry_entry_edge_v0, sm_vtx_cnt);
            if (registry_entry_edge_is_from_cutmesh) {
                // ... then intersected face was from the source mesh
                tested_face = std::find_if(registry_entry_faces.cbegin(), registry_entry_faces.cend(),
                    [&](const fd_t& f) {
                        bool is_sm_face = !ps_is_cutmesh_face(f, sm_face_count) && f != hmesh_t::null_face();
                        return is_sm_face;
                    });
            } else {
                // ... then intersected face was from the cut mesh
                tested_face = std::find_if(registry_entry_faces.cbegin(), registry_entry_faces.cend(),
                    [&](const fd_t& f) {
                        bool is_cm_face = ps_is_cutmesh_face(f, sm_face_count) && f != hmesh_t::null_face();
                        return is_cm_face;
                    });
            }

            MCUT_ASSERT(tested_face != registry_entry_faces.cend()); // "registry_entry_faces" must have at least one face from cm and at least one from sm
            MCUT_ASSERT(ps_tested_face_to_plane_normal.find(*tested_face) != ps_tested_face_to_plane_normal.cend());

            // get normal of face
            const vec3& polygon_normal = SAFE_ACCESS(ps_tested_face_to_plane_normal, *tested_face);
            // const vec3& polygon_normal = SAFE_ACCESS(m0_ivtx_to_tested_polygon_normal, sm_poly_he_tgt);
            // const vec3& polygon_normal = geometric_data.first;
            // const double& orig_scalar_prod = geometric_data.second;

            // MCUT_ASSERT(sign(orig_scalar_prod) == NEGATIVE);

            const vec3 sm_poly_he_vector = m0.vertex(sm_poly_he_tgt) - m0.vertex(sm_poly_he_src);
            const double scalar_prod = dot_product(polygon_normal, sm_poly_he_vector);

            // Again, the notion of exterior is denoted by a negative dot-product.
            // Original ps-halfedge was "incoming" and gave a negative scalar-product
            // with the cut-mesh face normal.
            //
            // We want the same sign (i.e. cs_poly_he_vector has negative scalar-product) because we want
            // the class-1 ihalfedge which is exterior but points "inside" the cut-mesh (i.e. torward
            // the negative side)
            if (sign(scalar_prod) == NEGATIVE) {

                // At this point, we have found our class-1 (or class 3, x-->x) source-mesh halfedge
                // from which we can infer whether the current polygon is "above" (outside) or
                // "below" (inside) the cut-mesh.
                // Also, by using the traced halfedge connectivity, we can determine the adjacent polygon
                // which shares an edge with the curent polygon. This shared edge is the edge of the
                // "next" halfedge (of the current halfedge) and it is always an interior edge.

                // Check if the current polygon is already found to be "above" the cut-mesh
                const bool cur_poly_already_marked_as_above = std::find(sm_polygons_above_cs.cbegin(), sm_polygons_above_cs.cend(), sm_poly_idx) != sm_polygons_above_cs.cend();

                if (!cur_poly_already_marked_as_above) {
                    sm_polygons_above_cs.push_back(sm_poly_idx);
                }

                // Here we can conviniently find and save the the neighbouring polygon that is on the
                // other side i.e. "below" the cut-mesh. This is made possible because we can easily
                // search through the halfedge connectivity.

                // index of current halfedge in the current polygon
                const int sm_poly_he_idx = (int)std::distance(sm_poly.cbegin(), sm_poly_he_iter);
                // index of the "next" halfedge in the current polygon
                const int sm_poly_next_he_idx = wrap_integer(sm_poly_he_idx + 1, 0, (int)sm_poly.size() - 1);
                // the handle of the next halfedge in the current polygon
                const hd_t& sm_poly_next_he = SAFE_ACCESS(sm_poly, sm_poly_next_he_idx);
                // now we query the handle of the opposite-halfedge of the next-halfedge.
                // This is facilitated by the incidence information that is maintained inside
                // our halfedge data structure "m0" which stores our vertices (including intersection
                // points) and edges that we calculated in earlier stages of the pipeline).
                const hd_t opp_of_sm_poly_next_he = m0.opposite(sm_poly_next_he);
                // using our halfedge-to-traced-polygon map, we then get the polygon index of the
                // opposite-halfedge
                const std::vector<int>& coincident_polys = SAFE_ACCESS(m0_h_to_ply, opp_of_sm_poly_next_he); // coincident polygons (one cs and one sm)
                const std::vector<int>::const_iterator find_iter = std::find_if(
                    coincident_polys.cbegin(), coincident_polys.cend(),
                    [&](const int& e) { return (e < traced_sm_polygon_count); });

                // must always exist since "opp_of_sm_poly_next_he" is an interior ihalfedge
                MCUT_ASSERT(find_iter != coincident_polys.cend());

                // we have found the other source-mesh polygon which is "below" (inside) the cut-mesh
                const int coincident_sm_poly_idx = *find_iter;
                const bool neigh_poly_already_marked_as_below = std::find(sm_polygons_below_cs.cbegin(), sm_polygons_below_cs.cend(), sm_poly_idx) != sm_polygons_below_cs.cend();

                if (!neigh_poly_already_marked_as_below) {
                    sm_polygons_below_cs.push_back(coincident_sm_poly_idx);
                }
            }
        }
    }

    // sort that we can do binary search over it
    std::sort(sm_polygons_below_cs.begin(), sm_polygons_below_cs.end());
    std::sort(sm_polygons_above_cs.begin(), sm_polygons_above_cs.end());

    TIMESTACK_POP();

    // sm_nonborder_reentrant_ivtx_list.clear();

    // Here, we check for the unique case in which we could not find any traced source-mesh
    // polygons along the cut path which could be identified as either "above" (outside)
    // or "below" (inside).
    // Such a situation is rare and happens when the source-mesh has one face where the
    // intersection with the cut-mesh is a partial cut

    // if (sm_polygons_above_cs.empty() && sm_polygons_below_cs.empty()) {
    //     MCUT_ASSERT(sm_face_count == 1);
    //     sm_polygons_above_cs.push_back(0); // sm polygons are stored first theirfore sm polygon will ccse first (see "ps" definition)
    //     sm_polygons_below_cs.push_back(0);
    // }

    TIMESTACK_PUSH("Map source mesh ihalfedges to bool");

    ///////////////////////////////////////////////////////////////////////////
    // Map source-mesh intersection halfedges to a boolean value
    ///////////////////////////////////////////////////////////////////////////

    //
    // Here we will map every source-mesh halfedge connected to an intersection point to a boolean.
    // This boolean value indicates if the halfedge has been `transformed`. The notion
    // of "transformation" is used to indicate whether a halfedge has been "processed"
    // to assign it to a distinct fragment connected component of the source-mesh.
    //
    // We call a halfedge connected to at-least one intersection point note an "intersection
    // halfedge"
    //

    std::unordered_map<
        hd_t, // intersection halfedge which is used for tracing
        bool // flag for indicating if halfedge has been transformed
        >
        m0_sm_ihe_to_flag;

    // for (edge_array_iterator_t edge_iter = m0.edges_begin(); edge_iter != m0.edges_end(); ++edge_iter) {
    for (std::unordered_map<vd_t, std::vector<hd_t>>::const_iterator ivtx_iter = ivtx_to_incoming_hlist.cbegin(); ivtx_iter != ivtx_to_incoming_hlist.cend(); ++ivtx_iter) {
        for (std::vector<hd_t>::const_iterator halfedge_iter = ivtx_iter->second.cbegin(); halfedge_iter != ivtx_iter->second.cend(); ++halfedge_iter) {
            // const ed_t& edge = (*edge_iter);
            const ed_t& edge = m0.edge(*halfedge_iter);
            const vd_t v0 = m0.vertex(edge, 0);
            const vd_t v1 = m0.vertex(edge, 1);

            const bool v0_is_ivtx = m0_is_intersection_point(v0, ps_vtx_cnt);
            const bool v1_is_ivtx = m0_is_intersection_point(v1, ps_vtx_cnt);

            if (!v0_is_ivtx && !v1_is_ivtx) { // o-->o
                // we only want halfedges with an intersection point
                continue;
            }

            //
            // check if current edge is a cut-mesh edge
            //

            if (v0_is_ivtx && !v1_is_ivtx) { // x-->o

                // get the polygon-soup version of tgt descriptor
                // std::map<vd_t, vd_t>::const_iterator m0_to_ps_vtx_find_v1_iter = std::find_if(
                //    m0_to_ps_vtx.cbegin(), m0_to_ps_vtx.cend(),
                //    [&](const std::pair<vd_t, vd_t> &e) { return e.first == v1; });

                MCUT_ASSERT((int)v1 < (int)m0_to_ps_vtx.size() /*m0_to_ps_vtx_find_v1_iter != m0_to_ps_vtx.cend()*/);

                const vd_t& ps_v1 = SAFE_ACCESS(m0_to_ps_vtx, v1); // m0_to_ps_vtx_find_v1_iter->second;

                if (ps_is_cutmesh_vertex(ps_v1, sm_vtx_cnt)) { // is it a cut-mesh vertex..?
                    // we want only source-mesh edges
                    continue;
                }
            }

            if (!v0_is_ivtx && v1_is_ivtx) { // o-->x
                // std::map<vd_t, vd_t>::const_iterator m0_to_ps_vtx_find_v0_iter = std::find_if(
                //     m0_to_ps_vtx.cbegin(), m0_to_ps_vtx.cend(),
                //     [&](const std::pair<vd_t, vd_t> &e) { return e.first == v0; });

                MCUT_ASSERT((size_t)v0 < m0_to_ps_vtx.size()); // m0_to_ps_vtx_find_v0_iter != m0_to_ps_vtx.cend());

                const vd_t& ps_v0 = SAFE_ACCESS(m0_to_ps_vtx, v0); // m0_to_ps_vtx_find_v0_iter->second;

                if (ps_is_cutmesh_vertex(ps_v0, sm_vtx_cnt)) {
                    continue; // is a cut-mesh edge
                }
            }

            // TODO: we also need to check for cut-mesh edges of the form x-->x [but only the polygon boundary type]
            // At the moment, "m0_sm_ihe_to_flag" will also include those cut-mesh halfedges!
            //
            // ** I'm not convinced that this is a problem

#if 0
        bool is_ambiguious_boundary_edge_case = v0_is_ivtx && v1_is_ivtx;

        if (is_ambiguious_boundary_edge_case) { // exterior edge with two intersection vertices (ambigious case arising from concave polyhedron cut)

          const hd_t v0_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, v0);
          const hd_t v1_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, v1);
          const ed_t v0_ps_edge = ps.edge(v0_coincident_ps_halfedge);
          const ed_t v1_ps_edge = ps.edge(v1_coincident_ps_halfedge);
          bool is_valid_ambiguious_boundary_edge = (v0_ps_edge == v1_ps_edge); // see also above when gathering exterior incident edges
          if (is_valid_ambiguious_boundary_edge && !cm_is_watertight) // x-->x where o-->x-->x-->o
          {
            // Exterior ihalfedges (and hence their respective halfedges) are not transformed.
            // Only interior ihalfedges need to be transformed to create incisions that allow openings of the sm via transformations.
            // NOTE: when cs is watertight we still include polygon-exterior interior-ihalfedge because they a needed to "bite" a chuck out of the cs (see example 19)
            continue;
          }
      }
#endif

            //
            // save the halfegdes of the current edge (if they are used to trace a polygon)
            //

            const hd_t h0 = m0.halfedge(edge, 0);

            if (SAFE_ACCESS(m0_h_to_ply, h0).size() > 0 /*m0_h_to_ply.find(h0) != m0_h_to_ply.end()*/ && m0_sm_ihe_to_flag.find(h0) == m0_sm_ihe_to_flag.cend()) { // check if used to trace polygon
                MCUT_ASSERT(m0_sm_ihe_to_flag.count(h0) == 0);
                m0_sm_ihe_to_flag[h0] = false;
                // std::pair<std::map<hd_t, bool>::const_iterator, bool> pair0 = m0_sm_ihe_to_flag.insert(std::make_pair(h0, false));
                // MCUT_ASSERT(pair0.second == true);
                MCUT_ASSERT(m0_sm_ihe_to_flag.count(h0) == 1);
            }

            const hd_t h1 = m0.halfedge(edge, 1);

            if (SAFE_ACCESS(m0_h_to_ply, h1).size() > 0 /*m0_h_to_ply.find(h1) != m0_h_to_ply.end()*/ && m0_sm_ihe_to_flag.find(h1) == m0_sm_ihe_to_flag.cend()) { // check id used to trace polygon
                MCUT_ASSERT(m0_sm_ihe_to_flag.count(h1) == 0);
                m0_sm_ihe_to_flag[h1] = false;
                // std::pair<std::map<hd_t, bool>::const_iterator, bool> pair1 = m0_sm_ihe_to_flag.insert(std::make_pair(h1, false));
                // MCUT_ASSERT(pair1.second == true);
                MCUT_ASSERT(m0_sm_ihe_to_flag.count(h1) == 1);
            }
        }
    }

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // create the second auxilliary halfedge data structure ("m1")
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Create m1");
    //
    // At this point, we create another auxilliary halfedge data structure called "m1".
    // It will store the vertices and edges like "m0" but will also include the
    // duplicate copy of (most/all) intersection points, as well as some new edges. The new
    // edges are created to partition/separate the source-mesh as we process intersection
    // halfedges by assigning them the correct copy of dupicated intersection
    // points. Thus along the cut path, we will create new connectivity that
    // allows us to partition the source-mesh along this path.
    //

    // store's the (unsealed) connected components (fragments of the source-mesh)
    hmesh_t m1;
    m1.reserve_for_additional_elements(m0.number_of_vertices() + m0.number_of_vertices() * 0.25);
    // copy vertices from m0 t0 m1 (and save mapping to avoid assumptions).
    // This map DOES NOT include patch intersection points because they are new
    // i.e. we keep only the points represent original vertices in the source-mesh
    // and cut-mesh (used as data for client tex coord mapping usage)
    std::vector<vd_t> m0_to_m1_vtx(m0.number_of_vertices());
    std::vector<vd_t> m1_to_m0_ovtx(m0.number_of_vertices());
    for (vertex_array_iterator_t v = m0.vertices_begin(); v != m0.vertices_end(); ++v) {
        const vd_t m1_vd = m1.add_vertex(m0.vertex(*v));
        MCUT_ASSERT(m1_vd != hmesh_t::null_vertex());

        MCUT_ASSERT((size_t)(*v) < m0_to_m1_vtx.size() /*m0_to_m1_vtx.count(*v) == 0*/ && (size_t)m1_vd < m1_to_m0_ovtx.size() /* m1_to_m0_ovtx.count(m1_vd) == 0*/);
        // std::pair<std::map<vd_t, vd_t>::const_iterator, bool> pair = m0_to_m1_vtx.insert(std::make_pair(*v, m1_vd));
        // MCUT_ASSERT(pair.second == true);
        m0_to_m1_vtx[*v] = m1_vd;
        m1_to_m0_ovtx[m1_vd] = *v;
        // MCUT_ASSERT(m0_to_m1_vtx.count(*v) == 1);
    }

    MCUT_ASSERT(m1.number_of_vertices() == m0.number_of_vertices());

    TIMESTACK_POP();

    TIMESTACK_PUSH("Map m0 to m1 halfedges");

    // copy m0 edges and halfedges [which are not intersection-halfedges] and
    // build a mapping between m0 and m1. This mapping is needed because as we
    // begin to transform halfedges incident to the cut-path, some of their opposites
    // will become invalidated. This is because for each interior edge we will
    // essentially create a new edge.
    // We must also relate halfedges (in "m1") to their opposites explicitly (essentially
    // copying the information already stored in "m0"). Because this information will be
    // lost after duplicating intersection points and transforming all halfedges
    // along the cut-path.

    std::unordered_map<hd_t, hd_t> m0_to_m1_he;
    m0_to_m1_he.reserve(m0.number_of_edges() * 2);

    for (edge_array_iterator_t e = m0.edges_begin(); e != m0.edges_end(); ++e) {
        const ed_t& m0_edge = (*e);
        const vd_t m0_v0 = m0.vertex(m0_edge, 0);
        const vd_t m0_v1 = m0.vertex(m0_edge, 1);
        // const bool m0_v0_is_ivtx = m0_is_intersection_point(m0_v0, ps_vtx_cnt);
        // const bool m0_v1_is_ivtx = m0_is_intersection_point(m0_v1, ps_vtx_cnt);

        if (!(m0_is_intersection_point(m0_v0, ps_vtx_cnt) || m0_is_intersection_point(m0_v1, ps_vtx_cnt))) { // not coincident to an intersection vertex (i.e. is class-0 edge)

            const vd_t m1_v0 = SAFE_ACCESS(m0_to_m1_vtx, m0_v0);
            const vd_t m1_v1 = SAFE_ACCESS(m0_to_m1_vtx, m0_v1);
            const hd_t m1_halfedge = m1.add_edge(m1_v0, m1_v1); // add m1
            const hd_t m0_h0 = m0.halfedge(m0_edge, 0);
            const vd_t m0_h0_src = m0.source(m0_h0);
            const hd_t m0_h1 = m0.halfedge(m0_edge, 1);
            const vd_t m1_halfedge_src = m1.source(m1_halfedge);
            const vd_t m1_halfedge_tgt = m1.target(m1_halfedge);

            if (SAFE_ACCESS(m0_to_m1_vtx, m0_h0_src) == m1_halfedge_src) { // i.e. "is the m0_h0 equivalent to m1_halfedge?"
                m0_to_m1_he.insert(std::make_pair(m0_h0, m1_halfedge));
                m0_to_m1_he.insert(std::make_pair(m0_h1, m1.opposite(m1_halfedge)));
            } else {
                m0_to_m1_he.insert(std::make_pair(m0_h1, m1_halfedge));
                m0_to_m1_he.insert(std::make_pair(m0_h0, m1.opposite(m1_halfedge)));
            }
        }
    }

    TIMESTACK_POP();

    TIMESTACK_PUSH("m0 source mesh set next");
    //
    // For each src-mesh halfedge we store "next-halfedge" state for quick-lookup in "m0".
    // We store this information in "m0" because it allows for a more expedient state-lookup during
    // connected-component re-assignment.
    //
    // Note: this is only made possible because 1) from this point onwards "m0" will not be modified
    // at all, and 2) we can safely assume that its okay to store "next-halfedge" state without `technically`
    // violating 2-manifold rules since this information is only for the src-mesh polygons.
    //
    // We will ultimately use this saved state to extract the intersection-halfedges which are coincident
    // to intersection-vertices during connected-component re-assignment.
    //

    // for each source-mesh polygon
    for (std::vector<traced_polygon_t>::const_iterator traced_sm_polygon_iter = m0_polygons.cbegin();
         traced_sm_polygon_iter != m0_traced_sm_polygons_iter_cend;
         ++traced_sm_polygon_iter) {
        const traced_polygon_t& traced_sm_polygon = *traced_sm_polygon_iter;

        // for each halfedge of polygon
        for (traced_polygon_t::const_iterator traced_sm_polygon_halfedge_iter = traced_sm_polygon.cbegin();
             traced_sm_polygon_halfedge_iter != traced_sm_polygon.cend();
             ++traced_sm_polygon_halfedge_iter) {
            const int i = (int)std::distance(traced_sm_polygon.cbegin(), traced_sm_polygon_halfedge_iter);

            const hd_t& cur = SAFE_ACCESS(traced_sm_polygon, i);
            const hd_t& next = SAFE_ACCESS(traced_sm_polygon, ((size_t)i + 1) % traced_sm_polygon.size());
            m0.set_next(cur, next); // update state
        }
    }

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // source-mesh partitioning
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Source mesh partitioning"); // &&&&&

    //
    // Here we partition the traced source-mesh polygons into disjoint connected components
    // by circulating around each intersection point and creating a copy for
    // each distinct connected component seen. In the case of a complete (through) cut,
    // all intersection points are duplicated. However, in the case of a partial
    // cut, we only duplicate the intersection points which are along the cut path
    // but exclude those which are the terminal vertices of a sequence.
    // A sequence here is an ordered list of edges passing through intersection
    // points (along the cut path). In the case of a partial cut, a sequence
    // does not form a loop.
    //

    /*
  Goal:
  Assign the correct intersection point instance (descriptor) to each intersection halfedge.

  For reference there following are the types of halfedges we have

  class : instance : definition (what will happen)

  0 : o-->o : Exterior/Boundary (nothing, we already copied this type of edge into m1)
  1 : o-->x : Exterior/Boundary (tgt may be assigned duplicate instance of descriptor)
  2 : x-->o : Exterior/Boundary (src may be assigned duplicate instance of descriptor)
  3 : x-->x : Interior OR Exterior/Boundary (tgt and src may be assigned duplicate instance of descriptor)

  o - original-vertex
  x - intersection-vertex/point
*/

    // This data structure will map the descriptors of intersection-halfedges in "m0"
    // to their descriptor in "m1". Thus, some halfedges (in "m0") will be mapped to
    // new halfedges which are not in "m0" but will be added into "m1".
    std::unordered_map<
        hd_t, // "m0" halfedge
        hd_t // "m1" version
        >
        m0_to_m1_ihe;

    // lamda checks the intersection halfedge that has not been transformed/processed already
    std::function<bool(const std::pair<hd_t, bool>&)> check_if_halfedge_is_transformed = [&](const std::pair<hd_t, bool>& e) {
        const hd_t& m0_ihe = e.first;
        const vd_t m0_ihe_src_vertex = m0.source(m0_ihe);
        const bool src_is_ivertex = m0_is_intersection_point(m0_ihe_src_vertex, ps_vtx_cnt);

        if (src_is_ivertex) {
            return false; // has to be original vertex
        }

        const bool is_transformed = e.second;

        if (is_transformed) {
            return false; // cannot have been transformed already.
        }

        const vd_t& m0_ihe_tgt_vertex = m0.target(m0_ihe);
        const bool tgt_is_ivertex = m0_is_intersection_point(m0_ihe_tgt_vertex, ps_vtx_cnt);

        if (tgt_is_ivertex) { // tgt is an intersection point

            // is the current halfedge used to traced a polygon (i.e. those we stored in "m0")
            const bool is_incident_to_traced_polygon = SAFE_ACCESS(m0_h_to_ply, m0_ihe).size() > 0 /*m0_h_to_ply.find(m0_ihe) != m0_h_to_ply.end()*/;

            if (is_incident_to_traced_polygon) {
                //
                // We now need to make sure that the preceeding (prevous) halfedge of the
                // current (in its polygon) is class0 or class2
                //

                // find coincident polygon
                const std::vector<int>& incident_polys = SAFE_ACCESS(m0_h_to_ply, m0_ihe);

                MCUT_ASSERT(incident_polys.size() == 1); // class-1 halfedges are incident to exactly one polygon

                const int incident_poly_idx = incident_polys.front();
                const traced_polygon_t& incident_poly = SAFE_ACCESS(m0_polygons, incident_poly_idx);
                // find the reference to the current halfedde (in the traced polygon)
                traced_polygon_t::const_iterator he_find_iter = std::find(incident_poly.cbegin(), incident_poly.cend(), m0_ihe);

                MCUT_ASSERT(he_find_iter != incident_poly.cend()); // if its incident to a polygon then that polygon must have it!

                // halfedge index in polygon
                const int he_index = (int)std::distance(incident_poly.cbegin(), he_find_iter);
                // index of previous halfedge in polygon
                const int preceeding_he_idx = wrap_integer(he_index - 1, 0, (int)incident_poly.size() - 1);
                const hd_t& preceeding_he = SAFE_ACCESS(incident_poly, preceeding_he_idx);
                const vd_t preceeding_he_src = m0.source(preceeding_he);
                const vd_t preceeding_he_tgt = m0.target(preceeding_he);
                const bool preceeding_he_src_is_ivertex = m0_is_intersection_point(preceeding_he_src, ps_vtx_cnt);
                const bool preceeding_he_tgt_is_ivertex = m0_is_intersection_point(preceeding_he_tgt, ps_vtx_cnt);
                // classify preceeding halfedge
                const bool preceeding_he_is_class0 = !preceeding_he_src_is_ivertex && !preceeding_he_tgt_is_ivertex; // o-->o
                const bool preceeding_he_is_class2 = preceeding_he_src_is_ivertex && !preceeding_he_tgt_is_ivertex; // x-->o
                // count the original vertices that are contained in the polygon the current halfedge.
                // we need this check to detect a special edge case.
                const int overtices_in_poly = (int)std::count_if(
                    incident_poly.cbegin(), incident_poly.cend(),
                    [&](const hd_t& e) { return !m0_is_intersection_point(m0.target(e), ps_vtx_cnt); });

                return (preceeding_he_is_class0 || (preceeding_he_is_class2 && overtices_in_poly == 1));
            }
        }

        return false;
    };

    // our routine will start from an untransformed class-1 intersection-halfedge. We do this because it makes
    // transformation process easier for us by reducing the number of steps.
    std::unordered_map<hd_t, bool>::iterator m0_1st_sm_ihe_fiter = std::find_if( // for each src-mesh intersection halfedge
        m0_sm_ihe_to_flag.begin(),
        m0_sm_ihe_to_flag.end(),
        check_if_halfedge_is_transformed);

    // Here we have queue of intersection halfedges which will be used to begin a transformation walk/traversal
    // around the polygon of each contained halfedge. For each polygon along the cut path there will ever be at
    // most one of its halfedges in this queue.
    // To assign a polygon along the cut-path to the correct connected component, we will traverse a subset (or full set)
    // of it halfedges in order to assign the correct instance of each referenced intersection point to the traversed
    // halfedge (s).
    std::vector<hd_t> m0_ox_hlist;

    // used to specifically prevent duplicate ox and xo halfedges
    // Map-key=an intersection-vertex in m1;
    // Map-value=list of (vertex,halfedge) pairs.
    //  The first elem in a pair is a vertex connected to [Map-key].
    //  The second element in a pair is the halfedge connecting [Map-key] and the first element in the pair
    //
    // NOTE: [Map-key] is always the src vertex of the halfedge which is the second element in a pair of [Map-value]
    std::unordered_map<
        vd_t, // "m1" intersection point
        std::vector< // list of (vertex,halfedge) pairs.
            std::pair<
                vd_t, // vertex connected to "m1" intersection point (i.e. the key of this map)
                hd_t // the halfedge connecting the vertices
                >>>
        m1_ivtx_to_h;

    // At this point we also introduce the notion of a so-called "strongly connected border set" (SCBS).
    // An SCBS is set of adjacent source-mesh polygons along the partitioned cut-path ("partitioned" implies that the
    // source-mesh polygons along the cut-path are no longer "topologically connected").
    // int strongly_connected_sm_boundary_seq_iter_id = -1;

    // for each strongly-connected set of sm-boundary sequences.
    // one iteration pertains to a transformation of a set of sm-boundary sequences which all belong to the same connected ccsponent.
    // sets of sm-boundary sequences which belong to the same (sm) connected component may be produced different iterations.

    do {
        //

        MCUT_ASSERT((m0_1st_sm_ihe_fiter != m0_sm_ihe_to_flag.end())); // their must be at least one halfedge from which we can start walking!

        m0_ox_hlist.push_back(m0_1st_sm_ihe_fiter->first); // add to queue

        // The following do-while loop will transform/process the halfedges which belong
        // to exactly one SCBS
        do {

            hd_t m0_cur_h = hmesh_t::null_halfedge();
            // get first intersection halfedge which determine's the first polygon of the current SCBS.
            // Note that the current SCBS is determined implicitely from this initial intersection halfedge.
            const hd_t seq_init_ihe = m0_ox_hlist.back();

            m0_ox_hlist.pop_back(); // remove "seq_init_ihe" from queue
            hd_t m0_nxt_h = seq_init_ihe;

            // The following do-while loop will transform/process the halfedges which belong
            // to [a part] of the current SCBS (i.e. a swept surface of polygons next to the
            // partitioned cut-path).
            do { // process ih sequence starting from "seq_init_ihe"

                m0_cur_h = m0_nxt_h; // current

                m0_nxt_h = m0.next(m0_cur_h); // next

                const vd_t m0_cur_h_src = m0.source(m0_cur_h);
                const vd_t m0_cur_h_tgt = m0.target(m0_cur_h);
                const bool m0_cur_h_src_is_ivtx = m0_is_intersection_point(m0_cur_h_src, ps_vtx_cnt);
                const bool m0_cur_h_tgt_is_ivtx = m0_is_intersection_point(m0_cur_h_tgt, ps_vtx_cnt);
                const bool m0_cur_h_is_ox = !m0_cur_h_src_is_ivtx && m0_cur_h_tgt_is_ivtx; // o-->x

                MCUT_ASSERT((size_t)m0_cur_h_src < m0_to_m1_vtx.size() /* m0_to_m1_vtx.find(m0_cur_h_src) != m0_to_m1_vtx.cend()*/);

                vd_t m1_cur_h_src = SAFE_ACCESS(m0_to_m1_vtx, m0_cur_h_src); // from m0 to m1 descriptor

                MCUT_ASSERT((size_t)m0_cur_h_tgt < m0_to_m1_vtx.size() /*m0_to_m1_vtx.find(m0_cur_h_tgt) != m0_to_m1_vtx.cend()*/);

                vd_t m1_cur_h_tgt = SAFE_ACCESS(m0_to_m1_vtx, m0_cur_h_tgt); // from m0 to m1 descriptor

                // o-->x OR x-->x
                if (m0_cur_h_tgt_is_ivtx) { // tgt vertex of current halfedge is an intersection point

                    //
                    // check if the next halfedge has been processed/transformed
                    //

                    // can we find the m1 version of the next halfedge
                    std::unordered_map<hd_t, hd_t>::const_iterator m1_nxt_h_fiter = m0_to_m1_ihe.find(m0_nxt_h);
                    const bool nxt_is_processed = m1_nxt_h_fiter != m0_to_m1_ihe.cend();

                    if (nxt_is_processed) {
                        // Since the next halfedge has been processed, we can simply set
                        // target vertex instance of the current ("m1") halfedge to the source of the
                        // next halfedge
                        m1_cur_h_tgt = m1.source(m1_nxt_h_fiter->second);
                    } else {
                        // otherwise, we need to determined the correct instance of the tgt
                        // vertex to be used (see paper for details)
                        m1_cur_h_tgt = resolve_intersection_point_descriptor(ps, m0, m1, m0_cur_h, m0_cur_h_tgt, m1_cur_h_tgt, m0_cur_h_is_ox,
                            m0_h_to_ply, ivtx_to_incoming_hlist, m0_sm_ihe_to_flag, m0_ivtx_to_intersection_registry_entry, m0_to_m1_ihe, m0_to_ps_vtx, ps_vtx_cnt, sm_vtx_cnt, sm_face_count, m0_num_cutpath_halfedges);
                    }
                }

                // x-->o OR x-->x
                if (m0_cur_h_src_is_ivtx) { // src vertex of current halfedge is an intersection point
                    if (m0_cur_h == seq_init_ihe) // is it the first halfedge of the current SCBS?
                    {
                        // get the opposite halfedge
                        const hd_t opp = m0.opposite(m0_cur_h); // NOTE: m0_cur_h_src == target(opp)
                        // we need to determined the correct instance of the src vertex to be used (see paper for details)
                        m1_cur_h_src = resolve_intersection_point_descriptor(ps, m0, m1, opp, m0_cur_h_src, m1_cur_h_src, m0_cur_h_is_ox,
                            m0_h_to_ply, ivtx_to_incoming_hlist, m0_sm_ihe_to_flag, m0_ivtx_to_intersection_registry_entry, m0_to_m1_ihe, m0_to_ps_vtx, ps_vtx_cnt, sm_vtx_cnt, sm_face_count, m0_num_cutpath_halfedges);
                    } else { // current halfedge is not the first halfedge of the current SCBS

                        // get the previous halfedge
                        const hd_t m0_prv_h = m0.prev(m0_cur_h);

                        // The previous halfedge must have been transformed since the current halfedge
                        // is not the first halfedge of the current SCBS. TODO: explain further for why
                        // this is true when halfedge is x-->x (i.e. exterior, and from scoop cut)
                        MCUT_ASSERT(m0_to_m1_ihe.find(m0_prv_h) != m0_to_m1_ihe.cend());

                        // get transformed instance of previous halfedge ("m1" version )
                        const hd_t m1_prv_h = SAFE_ACCESS(m0_to_m1_ihe, m0_prv_h);
                        // Since the previous halfedge has been processed, we can simply set
                        // src vertex instance of the current ("m1") halfedge to the source of the
                        // next halfedge
                        const vd_t m1_prv_h_tgt = m1.target(m1_prv_h);
                        m1_cur_h_src = m1_prv_h_tgt;
                    }
                }

                //
                // Now that we have resolved the correct descriptor instance to use for the src and/or tgt vertex,
                // we will create an edge in halfedge data structure ("m1") connecting "m1_cur_h_src" and
                // "m1_cur_h_tgt" if this edge does not already exist.
                //

                // bool m1_cur_h_exists = false;
                // const bool m0_cur_h_is_xx = m0_cur_h_src_is_ivtx && m0_cur_h_tgt_is_ivtx;
                // std::map<vd_t, std::vector<std::pair<vd_t, hd_t> > >::iterator fiter = m1_ivtx_to_h.end();

                const bool is_boundary_halfedge = m0_is_polygon_boundary_halfedge(m0_cur_h, m0_num_cutpath_halfedges);
                bool m0_cur_h_is_exterior = is_boundary_halfedge;

#if 0
                    if (m0_cur_h_is_xx)
                    {
                        //const hd_t m0_cur_h_src_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_cur_h_src);
                        //const hd_t m0_cur_h_tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_cur_h_tgt);
                        MCUT_ASSERT((size_t)m0_cur_h_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_cur_h_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t> &m0_cur_h_src_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_cur_h_src - ps_vtx_cnt);
                        const ed_t m0_cur_h_src_ps_e = m0_cur_h_src_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_cur_h_src); //ps.edge(m0_cur_h_src_ps_h);

                        MCUT_ASSERT((size_t)m0_cur_h_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_cur_h_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t> &m0_cur_h_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_cur_h_tgt - ps_vtx_cnt);
                        const ed_t m0_cur_h_tgt_ps_e = m0_cur_h_tgt_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_cur_h_tgt); // ps.edge(m0_cur_h_tgt_ps_h);
                        m0_cur_h_is_exterior = (m0_cur_h_src_ps_e == m0_cur_h_tgt_ps_e);
                    }
#endif
                // get the opposite of the current halfedge ("m0")
                const hd_t opp = m0.opposite(m0_cur_h);
                // check if this opposite halfedge was used to traced a polygon
                const bool opp_used_for_tracing = SAFE_ACCESS(m0_h_to_ply, opp).size() > 0 /*m0_h_to_ply.find(opp) != m0_h_to_ply.end()*/;

                // if 1) the current halfedge is an interior halfedge (x-->x), OR
                // 2) the  current halfedge is an exterior halfedge AND it has not been processed
                if (!m0_cur_h_is_exterior || (m0_cur_h_is_exterior && m0_to_m1_ihe.find(m0_cur_h) == m0_to_m1_ihe.cend())) {

                    // create processed version (i.e. "m1" version) of "m0_cur_h"
                    hd_t m1_cur_h = m1.add_edge(m1_cur_h_src, m1_cur_h_tgt);

                    MCUT_ASSERT(m1_cur_h != hmesh_t::null_halfedge());

                    //
                    // here, we update the list containing the vertices and halfedge that are connected
                    // to the "m1_cur_h_src" and "m1_cur_h_tgt" i.e. adjacency information
                    //

                    if (m0_cur_h_is_exterior) { // is the current halfedge an exterior halfedge..?

                        // find entry in the adjancency vector

                        // src
                        std::unordered_map<vd_t, std::vector<std::pair<vd_t, hd_t>>>::iterator fiter = m1_ivtx_to_h.find(m1_cur_h_src);

                        if (fiter != m1_ivtx_to_h.cend()) // check src's entry exists
                        {
                            MCUT_ASSERT(std::find_if(fiter->second.cbegin(), fiter->second.cend(), [&](const std::pair<vd_t, hd_t>& p) { return p.first == m1_cur_h_tgt; }) == fiter->second.cend());
                            fiter->second.emplace_back(m1_cur_h_tgt, m1_cur_h); // record connection and save connecting halfedge
                        } else {
                            std::pair<std::unordered_map<vd_t, std::vector<std::pair<vd_t, hd_t>>>::iterator, bool> p = m1_ivtx_to_h.emplace(m1_cur_h_src, std::vector<std::pair<vd_t, hd_t>>());
                            MCUT_ASSERT(p.second == true);
                            fiter = p.first;
                            fiter->second.emplace_back(m1_cur_h_tgt, m1_cur_h);
                        }

                        // repeat for tgt

                        fiter = m1_ivtx_to_h.find(m1_cur_h_tgt); // check if the tgt's entry exists and update it

                        if (fiter != m1_ivtx_to_h.cend()) // check tgt's entry exists
                        {
                            MCUT_ASSERT(std::find_if(fiter->second.cbegin(), fiter->second.cend(), [&](const std::pair<vd_t, hd_t>& p) { return p.first == m1_cur_h_src; }) == fiter->second.cend());
                            fiter->second.emplace_back(m1_cur_h_src, m1.opposite(m1_cur_h)); // record that it is connected to src (by the opp he of m1_cur_h)
                        } else {
                            std::pair<std::unordered_map<vd_t, std::vector<std::pair<vd_t, hd_t>>>::iterator, bool> p = m1_ivtx_to_h.emplace(m1_cur_h_tgt, std::vector<std::pair<vd_t, hd_t>>());
                            MCUT_ASSERT(p.second == true);
                            fiter = p.first;
                            fiter->second.emplace_back(m1_cur_h_src, m1.opposite(m1_cur_h)); // record that it is connected to src (by the opp he of m1_cur_h)
                        }
                    }

                    // map m0 to m1 version of current halfedge
                    m0_to_m1_ihe.emplace(m0_cur_h, m1_cur_h);

                    // if 1) the current halfedge is an exterior halfedge, AND
                    // 2) the opposite of the current halfedge was used to trace a polygon
                    if (m0_cur_h_is_exterior && opp_used_for_tracing) {
                        // Thanks of the halfedge data structure (each edge has 2 halfedges),
                        // we also have the m1 version/copy of opposite halfedge.
                        // NOTE however, the opposite halfedge it is still not "processed", and
                        // we will do so only when we traverse/walk it!
                        m0_to_m1_ihe.emplace(opp, m1.opposite(m1_cur_h));
                    }

                    // NOTE: keep in mind that two opposite halfedges which are interior halfedges
                    // will belong to separate connected components after all processing is complete.
                    // (exterior halfedges on the other hand will share the same connected component
                    // as their opposites).
                } else {

                    //
                    // here, we have an exterior halfedge whose "m1" version has already been created.
                    //
                    MCUT_ASSERT(m0_to_m1_ihe.find(m0_cur_h) != m0_to_m1_ihe.cend());
                    const hd_t m1_cur_h = SAFE_ACCESS(m0_to_m1_ihe, m0_cur_h);
                }

                //
                // update queue of ox halfedges which will be the initial halfedges of
                // (potentially parts of) SCBS's to be processed
                //

                // x-->o
                const bool m0_cur_h_is_xo = m0_cur_h_src_is_ivtx && !m0_cur_h_tgt_is_ivtx;

                // if 1) curreent halfedge is x-->o AND 2) it's opposites has been using to trace a polygon, AND
                // 3) this opposite has not already been processed
                if (m0_cur_h_is_xo && opp_used_for_tracing && !SAFE_ACCESS(m0_sm_ihe_to_flag, opp)) {

                    // get the next halfedge
                    const hd_t nxt = m0.next(m0_cur_h);
                    // is the next halfedge an intersection halfedge "o-->x"
                    const bool nxt_is_ih = m0_is_intersection_point(m0.target(nxt), ps_vtx_cnt); // check if is last halfedge

                    if ((nxt_is_ih && nxt == seq_init_ihe) || !nxt_is_ih) {
                        // here we add the next SCBS's first halfedge from which SCBS processing will begin.
                        m0_ox_hlist.push_back(opp);
                    }
                }

                MCUT_ASSERT(m0_sm_ihe_to_flag.find(m0_cur_h) != m0_sm_ihe_to_flag.cend());

                SAFE_ACCESS(m0_sm_ihe_to_flag, m0_cur_h) = true; // mark as "processed"

            } while (
                // "next" is ihalfedge
                (m0_is_intersection_point(m0.source(m0_nxt_h), ps_vtx_cnt) || m0_is_intersection_point(m0.target(m0_nxt_h), ps_vtx_cnt)) &&
                // "next" is not transformed. For case when ihalfedge-sequence forms a loop.
                SAFE_ACCESS(m0_sm_ihe_to_flag, m0_nxt_h) == false); // TODO: I think this last condition is the same as "m0_nxt_h" == "seq_init_ihe" (try it bcz using m0_sm_ihe_to_flag will be slower)

        } while (!m0_ox_hlist.empty());

        //
        // find next class1 halfedge which has not been transformed ( possibly in the same connected ccsponent )
        //

        m0_1st_sm_ihe_fiter = std::find_if( // find o-->x halfedge
            m0_sm_ihe_to_flag.begin(),
            m0_sm_ihe_to_flag.end(),
            check_if_halfedge_is_transformed);

        // True only if there exists a src-mesh ps-edge which has [at least] two intersection points
        // This means that the source-mesh has a scoop cut (see example 19)
        const bool class1_ihalfedge_found = (m0_1st_sm_ihe_fiter != m0_sm_ihe_to_flag.end());

        if (!class1_ihalfedge_found) { // The above search failed to find an untransformed class-1 halfedge.

            //
            // So now we instead try to search an untransformed polygon-boundary interior-ihalfedge (x-->x).
            //

            m0_1st_sm_ihe_fiter = std::find_if( // for each intersection halfedge
                m0_sm_ihe_to_flag.begin(), m0_sm_ihe_to_flag.end(),
                [&](const std::pair<hd_t, bool>& e) {
                    const bool is_transformed = e.second; // has it already been transformed..?

                    if (is_transformed) {
                        return false; // we want only the transformed intersection halfedges
                    }

                    const hd_t& m0_ihe = e.first;

                    const vd_t m0_ihe_src_vertex = m0.source(m0_ihe);
                    const bool src_is_ivertex = m0_is_intersection_point(m0_ihe_src_vertex, ps_vtx_cnt);
                    const vd_t m0_ihe_tgt_vertex = m0.target(m0_ihe);
                    const bool tgt_is_ivertex = m0_is_intersection_point(m0_ihe_tgt_vertex, ps_vtx_cnt);

                    if (!(src_is_ivertex && tgt_is_ivertex)) {
                        return false; // we want only class-3 intersection halfedges (x-->x)
                    }

//
// checf is halfedge is really an exterior one (ambigious case arising from concave polyhedron cut)
//
// const hd_t v0_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_ihe_src_vertex);
// const hd_t v1_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_ihe_tgt_vertex);
#if 0
                        MCUT_ASSERT((size_t)m0_ihe_src_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_ihe_src_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t> &m0_ihe_src_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_ihe_src_vertex - ps_vtx_cnt);
                        const ed_t v0_ps_edge = m0_ihe_src_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_ihe_src_vertex); //ps.edge(v0_coincident_ps_halfedge);

                        MCUT_ASSERT((size_t)m0_ihe_tgt_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_ihe_tgt_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t> &m0_ihe_tgt_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, m0_ihe_tgt_vertex - ps_vtx_cnt);
                        const ed_t v1_ps_edge = m0_ihe_tgt_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_ihe_tgt_vertex); // ps.edge(v1_coincident_ps_halfedge);

                        const bool is_poly_exterior_interior_ihalfedge = (v0_ps_edge == v1_ps_edge);
#endif
                    const bool is_boundary_halfedge = m0_is_polygon_boundary_halfedge(m0_ihe, m0_num_cutpath_halfedges);

                    if (is_boundary_halfedge /*is_poly_exterior_interior_ihalfedge*/) { // we want only polygon-exterior interior ihalfedges

                        // get the traced polygon which uses the current halfedge
                        const std::vector<std::vector<int>>::const_iterator coincident_poly_find_iter = m0_h_to_ply.cbegin() + m0_ihe; /* m0_h_to_ply.find(m0_ihe);*/

                        MCUT_ASSERT(coincident_poly_find_iter != m0_h_to_ply.end());
                        MCUT_ASSERT(coincident_poly_find_iter->size() == 1); // polygon-exterior interior-ihalfedges are incident to exactly one polygon

                        const bool is_used_to_trace_src_mesh_polygon = (coincident_poly_find_iter->front() < traced_sm_polygon_count);

                        return (is_used_to_trace_src_mesh_polygon);
                    } else {
                        return false;
                    }
                });
        }

        // loop while there exists a "non-transformed" exterior intersection-halfedge
        // from which we can start building a SCBS
    } while (m0_1st_sm_ihe_fiter != m0_sm_ihe_to_flag.end());

#if 0
    // dump
    

    for (std::map<hd_t, hd_t>::const_iterator i = m0_to_m1_ihe.cbegin(); i != m0_to_m1_ihe.cend(); ++i) {
        
    }
#endif

    TIMESTACK_POP(); // &&&&&

    // m0_to_ps_vtx.clear(); // free
    ivtx_to_incoming_hlist.clear(); // free
    m0_sm_ihe_to_flag.clear(); // free
    // m0_to_m1_vtx.clear(); // free
    m0_ox_hlist.clear(); // free
    m1_ivtx_to_h.clear(); // free

    //
    // NOTE: at this stage, we have calculated all the vertices, edges, halfedges and meta-data
    // which describes the connectivity of the partitioned (topologically split) source-mesh along
    // the cut-path.
    //

    ///////////////////////////////////////////////////////////////////////////
    // Save the number of vertices in "m1" after source-mesh partitioning
    ///////////////////////////////////////////////////////////////////////////

    // saving the number of vertices here will allow us to infer exactly which vertices
    // lie on the seam.

    const int m1_num_vertices_after_srcmesh_partitioning = m1.number_of_vertices();

    ///////////////////////////////////////////////////////////////////////////
    // Update the traced polygons to represent the partitioned src-mesh
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Update traced polygons");

    //
    // We are basically re-tracing the polygons that we traced earlier (in "m0").
    // These retraced polygon are stored in "m1". The re-traced polygons which
    // where next to the cut-path will now reside (i.e. reference vertices and
    // halfedges) in the correct connected component to separate the source mesh.
    //

    // the updated polygons (with the partitioning)
    std::vector<traced_polygon_t> m1_polygons;
    m1_polygons.resize(traced_sm_polygon_count); // resize to match

    // NOTE: this map contains polygons which are traced on the source mesh only.
    // We do this because cut-mesh polygon will have two version each, where
    // a version corresponds to those that are stitched on the exterior and those interior.
    // We will duplicate the "m1" mesh later into two copies.
    std::unordered_map<int, int> m0_to_m1_face; // std::map<int, int> m0_to_m1_face;
    std::unordered_map<int, int> m1_to_m0_face;

    // for each traced polygon (in "m0")
    for (std::vector<traced_polygon_t>::const_iterator m0_traced_sm_polygon_iter = m0_polygons.cbegin();
         m0_traced_sm_polygon_iter != m0_traced_sm_polygons_iter_cend;
         ++m0_traced_sm_polygon_iter) {
        const traced_polygon_t& m0_sm_polygon = *m0_traced_sm_polygon_iter; // m0 version (unpartitioned)
        // get index of polygon
        const int polygon_index = (int)std::distance(m0_polygons.cbegin(), m0_traced_sm_polygon_iter);

        MCUT_ASSERT(polygon_index < (int)m1_polygons.size()); // sanity check

        traced_polygon_t& m1_sm_polygon = SAFE_ACCESS(m1_polygons, polygon_index); // m1 version (partitioned)
        m1_sm_polygon.resize(m0_sm_polygon.size()); // resize to match

        // for each halfedge of current polygon
        for (traced_polygon_t::const_iterator m0_traced_sm_polygon_halfedge_iter = m0_sm_polygon.cbegin();
             m0_traced_sm_polygon_halfedge_iter != m0_sm_polygon.cend();
             ++m0_traced_sm_polygon_halfedge_iter) {

            const hd_t& m0_he = *m0_traced_sm_polygon_halfedge_iter;
            const bool m0_he_src_is_ivertex = m0_is_intersection_point(m0.source(m0_he), ps_vtx_cnt);
            const bool m0_he_tgt_is_ivertex = m0_is_intersection_point(m0.target(m0_he), ps_vtx_cnt);
            // is the halfedge connected to an intersection point...?
            const bool is_ihalfedge = m0_he_src_is_ivertex || m0_he_tgt_is_ivertex;

            hd_t m1_he = hmesh_t::null_halfedge();

            if (is_ihalfedge) { // its an intersection halfedge
                MCUT_ASSERT(m0_to_m1_ihe.find(m0_he) != m0_to_m1_ihe.cend()); // must have been walked/traversed

                m1_he = SAFE_ACCESS(m0_to_m1_ihe, m0_he); // m1 version
            } else {
                MCUT_ASSERT(m0_to_m1_he.find(m0_he) != m0_to_m1_he.cend());

                m1_he = SAFE_ACCESS(m0_to_m1_he, m0_he); // m1 version
            }

            // get halfedge index in polygon
            const int halfedge_index = (int)std::distance(m0_sm_polygon.cbegin(), m0_traced_sm_polygon_halfedge_iter);

            MCUT_ASSERT(halfedge_index < (int)m1_sm_polygon.size()); // array was resized with the same capacity as m0 polygon

            SAFE_ACCESS(m1_sm_polygon, halfedge_index) = m1_he;
        }

        m0_to_m1_face[polygon_index] = polygon_index; // one to one mapping because we are only dealing with source-mesh traced polygons
        m1_to_m0_face[polygon_index] = polygon_index;
    }

    m0_to_m1_he.clear();

    TIMESTACK_POP();

    //
    // NOTE: at this stage "m1_polygons" (and "m0_to_m1...") contains only source-mesh polygons.
    //

    TIMESTACK_PUSH("Mark seam edges");
    // extract the seam vertices
    // NOTE: the size of this vector include only ps vertices, intersection points, and
    // the duplicates of intersection points (for separating the source mesh).
    // new vertices that will later be created (by duplicate ps cut-mesh vertices during stitching)
    // are not included
    std::vector<bool> m1_vertex_to_seam_flag;
    mark_seam_vertices(m1_vertex_to_seam_flag, m1, ps_vtx_cnt, m1_num_vertices_after_srcmesh_partitioning);

    TIMESTACK_POP();

    MCUT_ASSERT(!m1_vertex_to_seam_flag.empty());

    if (input.keep_unsealed_fragments || input.keep_fragments_partially_cut) {
        ///////////////////////////////////////////////////////////////////////////
        // Extract the partitioned connected components for output
        ///////////////////////////////////////////////////////////////////////////

        std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>> unsealed_connected_components;

        extract_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            *input.scheduler,
#endif
            unsealed_connected_components,
            m1,
            0,
            m1_polygons,
            sm_polygons_below_cs,
            sm_polygons_above_cs,
            m1_vertex_to_seam_flag,
            m1_to_m0_ovtx,
            std::unordered_map<vd_t, vd_t>(), // ... because data is only available during "m1" stitching stage (later), and its not needed here
            m1_to_m0_face,
            m0_to_ps_vtx,
            m0_to_ps_face,
            ps_to_sm_vtx,
            ps_to_sm_face,
            ps_to_cm_vtx,
            ps_to_cm_face,
            sm_vtx_cnt,
            sm_face_count,
            input.populate_vertex_maps,
            input.populate_face_maps,
            input.keep_fragments_below_cutmesh,
            input.keep_fragments_above_cutmesh,
            input.keep_fragments_partially_cut);

        // for each connected component (i.e. mesh)
        for (std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>::iterator cc_iter = unsealed_connected_components.begin();
             cc_iter != unsealed_connected_components.end();
             ++cc_iter) {

            const int cc_id = static_cast<int>(cc_iter->first);
            std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>& mesh_data = cc_iter->second;

            // there will only be one element of the mesh since "unsealed_connected_components"
            // is empty before calling "extract_connected_components"
            MCUT_ASSERT(mesh_data.size() == 1);
            if (input.verbose) {
                dump_mesh(mesh_data.front().first.get()[0], ("fragment.unsealed." + std::to_string(cc_id) + "." + to_string(mesh_data.front().second.location)).c_str());
            }
            std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>& md = mesh_data.front();
            std::shared_ptr<output_mesh_info_t> omi = std::shared_ptr<output_mesh_info_t>(new output_mesh_info_t);
            omi->mesh = md.first;
            omi->seam_vertices = std::move(md.second.seam_vertices);
            omi->data_maps = std::move(md.second.data_maps);
            output.unsealed_cc[md.second.location].emplace_back((omi));
        }

        unsealed_connected_components.clear();
    }

    ///////////////////////////////////////////////////////////////////////////
    // Check if the pipeline needs to terminate at this point
    ///////////////////////////////////////////////////////////////////////////

    //
    // Note that  we do not ever continue to fill holes if the cut-mesh cuts
    // the source-mesh multiple times where most cuts are complete but there is at-least one partial.
    // So long as there is a partial cut and the input mesh is not water tight, we wont patch.
    // This is because patching becomes complex as we then need to account for skipping the task of stitching patches which are incident to hole-bounding-sequences which are not loops.
    // Maybe future work..?
    const bool proceed_to_fill_holes = explicit_cutpaths_making_holes.size() == m0_cutpath_sequences.size();

    //
    // The pipeline stops here if there are no holes to fill.
    //
    // NOTE: non-watertight meshes with a complete cut by a single cut-surface polygon cannot be "sealed"
    // since no edges of the cut-mesh polygon will be intersected.
    //

    if (proceed_to_fill_holes == false) {
        printf("[mcut-kernel]: detected a configuration that does not permit filling holes. input-mesh verification advised.\n");
        return; // done
    }

    if (false == (input.keep_fragments_below_cutmesh || //
            input.keep_fragments_above_cutmesh || //
            input.keep_fragments_sealed_inside || //
            input.keep_fragments_sealed_outside || input.keep_fragments_sealed_inside_exhaustive || //
            input.keep_fragments_sealed_outside_exhaustive || //
            input.keep_inside_patches || //
            input.keep_outside_patches)) {
        // if the user simply wants [unsealed] fragments that may be [partially cut], then we should not have to proceed further.
        return;
    }

    ///////////////////////////////////////////////////////////////////////////
    // The remainder of the pipeline performs hole-filling if there exists
    // atleast one from a circular/linear cut-path which creates a hole in the
    // source-mesh.
    // Specifically, we will seal each connected component by
    // identifying and then "stitching" patches of cut-mesh polygons to the
    // source-mesh connected components
    ///////////////////////////////////////////////////////////////////////////

    // Please refer to: `docs/notes.md`, section "Hole Filling"

    ///////////////////////////////////////////////////////////////////////////
    // Gather a primary intersection-halfedge for each patch (i.e. cut-path)
    ///////////////////////////////////////////////////////////////////////////

    // Here, we gather one interior intersection-halfedge (in "m0") for each patch,
    // (this halfedge is used to trace a cut-mesh polygon). We will use these
    // halfedges as starting points to iteratively build patches. Building a patch
    // is analogous to labelling each cut-mesh polygon with a patch id. We use halfedges along
    // cut-paths since they mark/represent the border/boundary between patches.
    //
    // Thus, in the following std::vector, each element is a pair of a 1) cut-mesh polygon index
    // and 2) the index of a halfedge (on a cut-path) in that polygon ("m0" version).
    //

    TIMESTACK_PUSH("Find primary halfedges for patch identification");

    std::vector<std::pair<int, int>> patch_discovery_seeds;

    // for each cutpath that makes a hole
    for (std::vector<int>::const_iterator ecpmh_iter = explicit_cutpaths_making_holes.cbegin();
         ecpmh_iter != explicit_cutpaths_making_holes.cend();
         ++ecpmh_iter) {

        const int ecpmh_idx = *ecpmh_iter; // index of cutpath
        MCUT_ASSERT(ecpmh_idx < (int)m0_cutpath_sequences.size());
        const std::vector<ed_t>& m0_explicit_cutpath_sequence = SAFE_ACCESS(m0_cutpath_sequences, ecpmh_idx);

        // pick any edge (we choose the first one)
        const ed_t& edge = m0_explicit_cutpath_sequence.front();
        MCUT_ASSERT(edge != hmesh_t::null_edge());

        for (int i = 0; i < 2; ++i) { // for each halfedge of current edge
            hd_t h = m0.halfedge(edge, i);
            MCUT_ASSERT(h != hmesh_t::null_halfedge());

            /*
         1. get the cut-mesh polygon using the halfedge
         2. save polygon and halfedge index
        */

            MCUT_ASSERT(SAFE_ACCESS(m0_h_to_ply, h).size() > 0 /*m0_h_to_ply.find(h) != m0_h_to_ply.cend()*/);
            const std::vector<int>& h_polygons = SAFE_ACCESS(m0_h_to_ply, h);

            // get the cut-mesh polygon using h0
            std::vector<int>::const_iterator h_cutmesh_polygon_find_iter = std::find_if(
                h_polygons.cbegin(),
                h_polygons.cend(),
                [&](const int& e) {
                    return e >= traced_sm_polygon_count; // match cutmesh polygon!
                });
            MCUT_ASSERT(h_cutmesh_polygon_find_iter != h_polygons.cend()); // cut path halfedge are always used by 2 polygons (for tracing)
            const int h_polygon_idx = *h_cutmesh_polygon_find_iter;
            // the actual polygon
            MCUT_ASSERT(h_polygon_idx < (int)m0_polygons.size());

            const std::vector<hd_t>& h_polygon = SAFE_ACCESS(m0_polygons, h_polygon_idx);
            // find the index of h0 in the polygon
            std::vector<hd_t>::const_iterator h_polygon_find_iter = std::find_if(
                h_polygon.cbegin(), h_polygon.cend(),
                [&](const hd_t& e) {
                    return e == h;
                });

            MCUT_ASSERT(h_polygon_find_iter != h_polygon.cend());

            const int h_idx = (int)std::distance(h_polygon.cbegin(), h_polygon_find_iter);

            patch_discovery_seeds.emplace_back(h_polygon_idx, h_idx); // save
        }
    }

    std::vector<std::pair<int, int>> primary_interior_ihalfedge_pool = patch_discovery_seeds; // copy because it gets modify

    TIMESTACK_POP();

    m0_cutpath_sequences.clear(); // free, no longer needed.

    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    // Find graph(s) and build patches
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Build patch graphs"); // &&&&&

    // Note that the built patches in this stage will have the same winding
    // order (e.g. counter-clock-wise ) as the input cut-mesh. The patches with
    // reversed winding order will be created later (once all patches are
    // identified).
    //
    // There is one graph which arises from the intersection of the
    // source-mesh with the cut-mesh. This graph is planar
    // and there is exactly one node with e.g. color "A" and the rest are "B".
    // When visualised, it looks like a star, and this graph topology
    // is bipartite. We call this a "strongly-connected set" (SCS) because there
    // is a path from one graph node to another.
    //
    //
    std::map<
        int, // patch index
        std::vector<int> // patch polygon-indices
        >
        patches;

    std::unordered_map<
        int, // traced cut-mesh polygon index
        int // patch index
        >
        m0_cm_poly_to_patch_idx;
    m0_cm_poly_to_patch_idx.reserve(cs_face_count);

    // This map stores an interior intersection-halfedge for each patch. This
    // halfedge also represents the traced cut-mesh polygon from which we will
    // start stitching/glueing the correspnding patch to a fragment connected
    // component of the source mesh.
    // This connected component will be one which has the same winding  as the
    // patch
    std::map<
        int, // patch index
        int // interior intersection-halfedge from which patch-stitching into a connected component will start from
        >
        patch_to_seed_interior_ihalfedge_idx;

    std::map<
        int, // patch index
        int> // index of patch-polygon that will be stitched first (from "patch_to_seed_interior_ihalfedge_idx[patch index]")
        patch_to_seed_poly_idx;

    // Interior/inside patches must be stitched into separate connected component
    // as exterior/outside patches so we create two versions of "m1" for that.
    //
    std::map<
        char, // color value (representing the notion of "interior"/"exterior")
        hmesh_t // the mesh (copy of "m1") to which corresponding patch(es) will be stitched
        >
        color_to_m1; // = { { 'A' /*e.g. "red"*/, m1 }, { 'B' /*e.g. "blue"*/, m1 } };
    color_to_m1.insert(std::make_pair('A' /*e.g. "red"*/, m1));
    color_to_m1.insert(std::make_pair('B' /*e.g. "red"*/, std::move(m1))); // "m1" becomes NULL after this

    // m1.reset(); // clear data

    // Patch (node) colors
    // NOTE: we use the letters 'A' and 'B' just to ensure lexicographical order
    // when we iterate over color_to_patch
    //
    std::map<
        char, // color value
        std::vector<int> // list of patches of that color
        >
        color_to_patch = { { 'A', std::vector<int>() }, { 'B', std::vector<int>() } };

    // We also tag each patch, identifying whether is it a floating patch or not.
    // All patches have an entry, including the reversed patches that are created
    // later.
    //
    // std::map<
    //    int, // patch id
    //    bool // flag to indicate of patch is a floating patch.
    //    >
    //    patch_to_floating_flag;

    // tracks how many cut-mesh polygons have been stitched. Used only for naming damped meshes
    int global_cm_poly_stitch_counter = 0;

    // keeps track of the total number of default-winding-order (e.g. CCW) patches which has been identified
    // NOTE: not all will be CCW if we have floating patches (in this case winding could be flipped)
    int total_ccw_patch_count = 0;
    std::vector<bool> patch_poly_enqueued(m0_polygons.size(), false);
    std::queue<int> flood_fill_queue; // for building patch using BFS

    do {
        ///////////////////////////////////////////////////////////////////////////
        // Associate cut-mesh polygons with patches of the graph
        ///////////////////////////////////////////////////////////////////////////

        int graph_cur_patch_idx = (int)patches.size();

        // counter to keep track of the number of patches discovered for
        // the current SCS

        //
        // Here, we will pick an interior intersection halfedge (and its polygon) from
        // which we can identify the [first patch] of the graph
        std::tuple<int, int, int> graph_interior_ihalfedge_pool;
        std::get<0>(graph_interior_ihalfedge_pool) = -1;

        // find the [first] interior intersection-halfedge of a cut-mesh polygon
        // which has not already been associated with a patch
        while (!primary_interior_ihalfedge_pool.empty()) { // while seeds are not found

            // pull an interior intersection-halfedge from the queue
            std::vector<std::pair<int, int>>::iterator primary_interior_ihalfedge_pool_citer = primary_interior_ihalfedge_pool.end() - 1; // last element
            // halfedge polygon index
            const int potential_seed_poly_idx = primary_interior_ihalfedge_pool_citer->first;
            // halfedge index in polygon
            const int potential_seed_poly_he_idx = primary_interior_ihalfedge_pool_citer->second;
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            const bool poly_patch_is_known = parallel_find_in_map_by_key(
                                                 *input.scheduler,
                                                 m0_cm_poly_to_patch_idx.cbegin(),
                                                 m0_cm_poly_to_patch_idx.cend(), potential_seed_poly_idx)
                != m0_cm_poly_to_patch_idx.cend();
#else
            // check if the polygon has already been associated with a patch
            const bool poly_patch_is_known = m0_cm_poly_to_patch_idx.find(potential_seed_poly_idx) != m0_cm_poly_to_patch_idx.cend();
#endif
            primary_interior_ihalfedge_pool_citer = primary_interior_ihalfedge_pool.erase(primary_interior_ihalfedge_pool_citer);

            if (!poly_patch_is_known) {
                // we can use the halfedge as a seed from which to starting point to build [a] patch
                std::get<0>(graph_interior_ihalfedge_pool) = graph_cur_patch_idx;
                std::get<1>(graph_interior_ihalfedge_pool) = potential_seed_poly_idx;
                std::get<2>(graph_interior_ihalfedge_pool) = potential_seed_poly_he_idx;
                break; // start patch discovery with the current seed
            }
        }

        if (std::get<0>(graph_interior_ihalfedge_pool) == -1) {
            break; // done
        }

        ///////////////////////////////////////////////////////////////////////////
        // build the patch by flood-fill (BFS)
        ///////////////////////////////////////////////////////////////////////////

        MCUT_ASSERT(patch_to_seed_interior_ihalfedge_idx.count(graph_cur_patch_idx) == 0);
        patch_to_seed_interior_ihalfedge_idx[graph_cur_patch_idx] = std::get<2>(graph_interior_ihalfedge_pool);

        MCUT_ASSERT(patch_to_seed_poly_idx.count(graph_cur_patch_idx) == 0);
        patch_to_seed_poly_idx[graph_cur_patch_idx] = std::get<1>(graph_interior_ihalfedge_pool);

        std::vector<int>& patch = patches[graph_cur_patch_idx]; // patch_insertion.first->second; // polygons of patch
        patch.reserve(cs_face_count);

        flood_fill_queue.push(std::get<1>(graph_interior_ihalfedge_pool)); // first polygon
        patch_poly_enqueued[std::get<1>(graph_interior_ihalfedge_pool)] = true;

        do { // each interation adds a polygon to the patch

            // get the polygon at the front of the queue
            const int graph_patch_poly_idx = flood_fill_queue.front();
            flood_fill_queue.pop(); // graph_patch_poly_idx

            // add polygon to patch
            patch.push_back(graph_patch_poly_idx);

            // relate polygon to patch
            MCUT_ASSERT(m0_cm_poly_to_patch_idx.count(graph_patch_poly_idx) == 0);
            m0_cm_poly_to_patch_idx[graph_patch_poly_idx] = graph_cur_patch_idx;
            // std::pair<std::map<int, int>::const_iterator, bool> pair = m0_cm_poly_to_patch_idx.insert(std::make_pair(graph_patch_poly_idx, graph_cur_patch_idx)); // signifies that polygon has been associated with a patch
            // MCUT_ASSERT(pair.second == true);
            MCUT_ASSERT(m0_cm_poly_to_patch_idx.count(graph_patch_poly_idx) == 1);

            //
            // find adjacent polygons which share class 0,1,2 (o-->o, o-->x, x-->o) halfedges, and
            // the class 3 (x-->x) halfedges which are [exterior/boundary] intersection-halfedges.
            //

            // the current polygon
            const traced_polygon_t& graph_patch_poly = SAFE_ACCESS(m0_polygons, graph_patch_poly_idx);

            // for each halfedge of the current polygon
            for (traced_polygon_t::const_iterator poly_he_iter = graph_patch_poly.cbegin();
                 poly_he_iter != graph_patch_poly.cend();
                 ++poly_he_iter) {

#if 0
                    const vd_t src_vertex = m0.source(*poly_he_iter);
                    const vd_t tgt_vertex = m0.target(*poly_he_iter);
                    bool is_ambiguious_boundary_edge_case = m0_is_intersection_point(src_vertex, ps_vtx_cnt) && m0_is_intersection_point(tgt_vertex, ps_vtx_cnt);
                    bool is_valid_ambiguious_boundary_edge = false;

                    if (is_ambiguious_boundary_edge_case)
                    { // exterior edge with two intersection vertices (ambigious case arising from concave polyhedron cut)

                        //const hd_t src_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, src_vertex);
                        //const hd_t tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, tgt_vertex);
                        MCUT_ASSERT((size_t)src_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(src_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t> &src_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, src_vertex - ps_vtx_cnt);
                        const ed_t src_ps_edge = src_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, src_vertex); //ps.edge(src_coincident_ps_halfedge);

                        MCUT_ASSERT((size_t)tgt_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(tgt_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t> &tgt_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, tgt_vertex - ps_vtx_cnt);
                        const ed_t tgt_ps_edge = tgt_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, tgt_vertex); //ps.edge(tgt_ps_h);

                        is_valid_ambiguious_boundary_edge = (src_ps_edge == tgt_ps_edge);
                    }
#endif
                const bool is_boundary_halfedge = m0_is_polygon_boundary_halfedge((*poly_he_iter), m0_num_cutpath_halfedges);

                // "is the halfdge not along the cut-path"
                if (is_boundary_halfedge /*!is_ambiguious_boundary_edge_case || is_valid_ambiguious_boundary_edge*/) {
                    // get the opposite halfedge which is used to trace the adjacent polygon
                    const hd_t poly_he_opp = m0.opposite(*poly_he_iter);
                    // get the coincident polygons (if any)
                    std::vector<std::vector<int>>::const_iterator find_iter = m0_h_to_ply.cbegin() + poly_he_opp; // m0_h_to_ply.find(poly_he_opp);

                    // check if "poly_he_opp" is used to trace a polygon i.e its not a border halfedge
                    if (find_iter->size() > 0 /*find_iter != m0_h_to_ply.cend()*/) {

                        MCUT_ASSERT(find_iter->size() == 1); // only used to trace a CCW cut-mesh polygon

                        // get the polygon which is traced with "poly_he_opp" i.e. the adjacent polygon we are looking for!
                        const int incident_poly = find_iter->front();

                        // mustbe  cut-mesh polygon since we are only dealing with such polygons when building patches
                        MCUT_ASSERT(incident_poly >= traced_sm_polygon_count);

                        // if (!poly_already_in_patch)
                        {
                            // std::unordered_map<int, bool>::const_iterator qmap_fiter = patch_poly_enqueued.find(incident_poly);
                            const bool poly_already_queued = patch_poly_enqueued[incident_poly]; // qmap_fiter != patch_poly_enqueued.cend(); //std::find(flood_fill_queue.crbegin(), flood_fill_queue.crend(), incident_poly) != flood_fill_queue.crend();
                            if (!poly_already_queued) {
                                flood_fill_queue.push(incident_poly); // add adjacent polygon to bfs-queue
                                patch_poly_enqueued[incident_poly] = true;
                            }
                        }
                    }
                }
            }

        } while (!flood_fill_queue.empty()); // while there are more adjacent polygons for building current patch

        MCUT_ASSERT(!patch.empty()); // there has to be at least one polygon

        // NOTE: at this stage, all patches/nodes of the current graph have been coloured i.e. we have bipartite graph of the patches (except if there is only one patch i.e. a floating patch).
    } while (!primary_interior_ihalfedge_pool.empty()); // while there are more interior ihalfedges coincident to polygons which have not been associated with a patch (hence, there are remaining graphs of patches to be stitched)

    patch_poly_enqueued.clear();

    // NOTE: At this stage, we have identified all patches of the current graph

    MCUT_ASSERT(patches.size() >= 1);

    total_ccw_patch_count = (int)patches.size();

    std::unordered_map<
        int, // patch index
        std::vector<int> // adjacent patches (i.e. sharing a cut-path)
        >
        graph_patch_to_adj_list;

    MCUT_ASSERT(patch_discovery_seeds.size() % 2 == 0);

    // To understand this loop see how "patch_discovery_seeds" is define above.
    // We are effetcively using the seed polygon for each patch, to determing adjacency.
    // "patch_discovery_seeds" is populated with two polygons are any one time,
    // where these polygons shared a cutpath edge, and hence belong to the boundaries
    // of their respective patches
    for (int i = 0; i < (int)patch_discovery_seeds.size(); i += 2) {
        const std::pair<int, int>& cur = patch_discovery_seeds[i];
        const int cur_cm_traced_poly = cur.first;
        const int cur_patch_idx = SAFE_ACCESS(m0_cm_poly_to_patch_idx, cur_cm_traced_poly);

        const std::pair<int, int>& nxt = patch_discovery_seeds[(std::size_t)i + 1];
        const int nxt_cm_traced_poly = nxt.first;
        const int nxt_patch_idx = SAFE_ACCESS(m0_cm_poly_to_patch_idx, nxt_cm_traced_poly);

        graph_patch_to_adj_list[cur_patch_idx].push_back(nxt_patch_idx);
        graph_patch_to_adj_list[nxt_patch_idx].push_back(cur_patch_idx);
    }

    ///////////////////////////////////////////////////////////////////////////
    // Identify which patches are interior and which are exterior (coloring)
    ///////////////////////////////////////////////////////////////////////////

    //
    // We will now sort the patches into two sets - interior and exterior.
    // We do this by building an adjacency matrix and its square (^2) to produce
    // a bipartite graph via coloring. The adjacency matrix represents the
    // adjacency between patches (sharing a cut path).
    //

    matrix_t<> scs_adj_matrix((int)patches.size(), (int)patches.size()); // square

    for (std::unordered_map<int, std::vector<int>>::const_iterator patch_iter = graph_patch_to_adj_list.cbegin();
         patch_iter != graph_patch_to_adj_list.cend();
         ++patch_iter) {

        const int row_id = patch_iter->first; // same as patch index

        for (std::vector<int>::const_iterator adj_patch_iter = patch_iter->second.cbegin();
             adj_patch_iter != patch_iter->second.cend();
             ++adj_patch_iter) {

            const int col_id = *adj_patch_iter;

            if (row_id == col_id) {
                // our adjacency matrix is no self referent because patches
                // do not connect to themselves!
                continue;
            }

            scs_adj_matrix(row_id, col_id) = 1; // mark adjacent
        }
    }

    const matrix_t<> scs_adj_matrix_sqrd = scs_adj_matrix * scs_adj_matrix;

    // Here we do graph coloring using BFS
    // NOTE: coloring is used to mark patches as either interior or exterior.
    // Be aware that since we work only with the topology (connectivity), the
    // notion color itself will not tell us whether a patch is interior or
    // exterior. The coloring simply tells us that a patch belongs to one
    // group or the other. One exception is when we have a floating-patch
    // in which case it is possible to infer that the patch is interior.
    // This is because floating patches are always defined by interior
    // intersection-halfedges.

    std::deque<int> graph_patch_coloring_queue;
    // start coloring with the first patch
    graph_patch_coloring_queue.push_back(0);
    // "red" chosen arbitrarilly
    std::vector<int>& red_nodes = SAFE_ACCESS(color_to_patch, 'A');

    do { // color the current node/patch of the red set
        const int graph_cur_colored_patch_idx = graph_patch_coloring_queue.front();
        red_nodes.push_back(graph_cur_colored_patch_idx);

        const int row_id = graph_cur_colored_patch_idx; // NOTE: no need to account for the fact that the number of patches accumulates since graph_1st_patch_idx == 0

        // find adjacent patch using A^2 and push adj patch onto queue (if not already colored)
        for (int col_id = 0; col_id < scs_adj_matrix_sqrd.cols(); ++col_id) {

            if (row_id == col_id) {
                continue; // we dont care about two-walks from a node back to itself
            }

            const unsigned int entry = scs_adj_matrix_sqrd(row_id, col_id);

            if (entry > 0) // two-walk exists
            {
                const int graph_next_colored_patch_idx = col_id;

                if ( // not already colored
                    std::find(red_nodes.cbegin(), red_nodes.cend(), graph_next_colored_patch_idx) == red_nodes.cend() &&
                    // not in queue
                    std::find(graph_patch_coloring_queue.cbegin(), graph_patch_coloring_queue.cend(), graph_next_colored_patch_idx) == graph_patch_coloring_queue.cend()) {
                    graph_patch_coloring_queue.push_back(graph_next_colored_patch_idx);
                }
            }
        }

        graph_patch_coloring_queue.pop_front(); // rm graph_cur_colored_patch_idx

    } while (!graph_patch_coloring_queue.empty());

    // color the remaining uncolored nodes
    std::vector<int>& blue_nodes = SAFE_ACCESS(color_to_patch, 'B'); // i.e. blue patches

    for (std::unordered_map<int, std::vector<int>>::const_iterator patch_iter = graph_patch_to_adj_list.cbegin();
         patch_iter != graph_patch_to_adj_list.cend();
         ++patch_iter) {

        const bool is_red = std::find(red_nodes.cbegin(), red_nodes.cend(), patch_iter->first) != red_nodes.cend();

        if (!is_red) {
            blue_nodes.push_back(patch_iter->first);
        }
    }

    TIMESTACK_POP(); // &&&&&

    // NOTE: at this stage, all strongly-connected-sets have been identified and colored (i.e via coloring, all nodes/patches have been associated with a side : interior or exterior)

    MCUT_ASSERT(!patches.empty());

    primary_interior_ihalfedge_pool.clear();

    ///////////////////////////////////////////////////////////////////////////
    // Find the cut-mesh vertices that must not be duplicated
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Find non-duplicated cut-mesh vertices");
    // In the case of a partial cut, the o-vertices of the cut-mesh are not duplicated
    // e.g. those which reside interior to the sm
    std::vector<vd_t> sm_interior_cs_border_vertices;

    if (partial_cut_detected) {

        //
        // Here we save the cut-mesh border vertices (non-intersection points) which
        // are on the interior (inside) of the src-mesh.
        // These are needed for calculating properly-sealed connected components which
        // have been partially cut. We use this informaion determine which vertices of
        // the cut-mesh to not duplicate while allowing for the openings in the sealed
        // the connected components.
        //
        MCUT_ASSERT(!cm_is_watertight);

        /*
        1. do while there exists a re-entrant vertex which has not been used to find an interior cut-mesh border vertices
            a. get the ihalfedge whose source is <1> and it is used for tracing
            b.  do while target of next halfedge along border is not an intersection point
                c. save target of current halfedge as one which we will not duplicate
                d. go to next halfedge along border
    */

        // populate the queue with all cut-mesh tip re-entrant vertices
        std::deque<vd_t> reentrant_ivertex_queue(cm_border_reentrant_ivtx_list.cbegin(), cm_border_reentrant_ivtx_list.cend());

        do {

            // pull any re-entrant vertex from queue
            const vd_t current_reentrant_ivertex = reentrant_ivertex_queue.front();

            hd_t current_cs_border_he = hmesh_t::null_halfedge();
            hd_t next_cs_border_he = hmesh_t::null_halfedge();
            int current_cs_border_he_idx = -1;
            int next_cs_border_he_idx = -1;

            //
            // find polygon, and its halfedge whose src vertex is the current re-entrant vertex
            // "current_reentrant_ivertex", and the target is not an intersection point
            // keep in mind that we are looking for the non-intersection points that lie
            // inside the src-mesh - so that we dont duplicate them.
            //
            std::vector<traced_polygon_t>::const_iterator next_cs_border_he_poly_find_iter = std::find_if(
                m0_polygons.cbegin() + traced_sm_polygon_count, // offset to start of traced cut-mesh polygons
                m0_polygons.cend(),
                [&](const traced_polygon_t& cs_poly) {
                    // for each halfedge of cut-mesh polygon
                    for (traced_polygon_t::const_iterator cs_poly_he_iter = cs_poly.cbegin();
                         cs_poly_he_iter != cs_poly.cend();
                         ++cs_poly_he_iter) {

                        // check if the target is an intersection point
                        const vd_t tgt = m0.target(*cs_poly_he_iter);
                        const bool tgt_is_ivertex = m0_is_intersection_point(tgt, ps_vtx_cnt);

                        if (tgt_is_ivertex) { //..-->x
                            continue;
                        }

                        // is the halfedge on the border of the cut-mesh i.e. its opposite halfedge is not
                        // used to trace a polygon
                        const bool is_on_cs_border = SAFE_ACCESS(m0_h_to_ply, m0.opposite(*cs_poly_he_iter)).size() == 0; // m0_h_to_ply.find(m0.opposite(*cs_poly_he_iter)) == m0_h_to_ply.cend(); // opposite is used to traced a polygon

                        if (is_on_cs_border) {

                            // check if the src vertex of the current halfedge is a re-entrant vertex
                            // we search through the queue because it contains the tip re-entrant vertices
                            // that have not yet been visited (valid set).
                            // Note that this implies that src is also an intersection point
                            const vd_t src = m0.source(*cs_poly_he_iter);
                            const bool src_is_reentrant = std::find(reentrant_ivertex_queue.cbegin(), reentrant_ivertex_queue.cend(), src) != reentrant_ivertex_queue.cend();

                            if (src_is_reentrant) {

                                // we have found that first halfedge from which the remaining one(s)
                                // inside the src-mesh can be found
                                next_cs_border_he = *cs_poly_he_iter;
                                next_cs_border_he_idx = (int)std::distance(cs_poly.cbegin(), cs_poly_he_iter);
                                break;
                            } else {
                                continue;
                            }
                        } else {
                            continue;
                        }
                    }

                    return (next_cs_border_he != hmesh_t::null_halfedge());
                });

            reentrant_ivertex_queue.pop_front(); // rm current_reentrant_ivertex

            // we could not find a halfedge whose src vertex is the current re-entrant vertex
            if (next_cs_border_he_poly_find_iter == m0_polygons.cend()) {
                // happens when a single cut-mesh partially cuts the src-mesh whereby
                // a single edge passes through 2 or more src-mesh faces e.g.
                // tet vs triangle partial cut
                continue;
            }

            // a reference to the polygon which is traced with the halfedge we found
            std::vector<traced_polygon_t>::const_iterator current_cs_border_he_poly_find_iter = m0_polygons.cend();

            //
            // we will now walk along the border of the cut-mesh saving all non
            // intersection points which must not be duplicated later
            //
            while (next_cs_border_he != hmesh_t::null_halfedge()) {

                // current border halfedge
                current_cs_border_he = next_cs_border_he;
                // polygon of current border halfedge
                current_cs_border_he_poly_find_iter = next_cs_border_he_poly_find_iter;
                // index of current border halfedge
                current_cs_border_he_idx = next_cs_border_he_idx;

                // reset
                next_cs_border_he = hmesh_t::null_halfedge();
                next_cs_border_he_idx = -1;
                next_cs_border_he_poly_find_iter = m0_polygons.cend();

                // save the non-intersection point on the border
                const vd_t current_cs_border_he_tgt = m0.target(current_cs_border_he);
                sm_interior_cs_border_vertices.push_back(current_cs_border_he_tgt);

                if (m0_is_intersection_point(current_cs_border_he_tgt, ps_vtx_cnt)) {
                    break; // done (finished walking along cut-mesh interior border)
                }

                //
                // find next halfedge along the border
                //

                // const int& current_cs_border_he_poly_idx = SAFE_ACCESS(m0_h_to_ply, current_cs_border_he).front(); // NOTE: class-2 or class-1 ihalfedges are incident to only one polygon

                // the current polygon
                // const traced_polygon_t& current_cs_border_he_poly = *current_cs_border_he_poly_find_iter;
                // get reference to the current border halfedge in the polygon
                // const traced_polygon_t::const_iterator current_cs_border_he_find_iter = std::find(
                //    current_cs_border_he_poly.cbegin(), current_cs_border_he_poly.cend(), current_cs_border_he);

                // halfedge must exist in the polygon because it is used for tracing
                // MCUT_ASSERT(current_cs_border_he_find_iter != current_cs_border_he_poly.cend());

                // const int current_cs_border_he_idx = std::distance(current_cs_border_he_poly.cbegin(), current_cs_border_he_find_iter);

                // Here we now find the next border halfedge
                // -----------------------------------------

                // We do this by circulating around "current_cs_border_he_tgt" to find the next border halfedge
                // starting from the next after the current halfedge (around the vertex)
                std::vector<traced_polygon_t>::const_iterator next_he_poly_iter = current_cs_border_he_poly_find_iter;
                std::vector<traced_polygon_t>::const_iterator cur_he_poly_iter = m0_polygons.cend();
                int next_he_idx = wrap_integer(current_cs_border_he_idx + 1, 0, (int)next_he_poly_iter->size() - 1);
                int cur_he_idx = -1;
                hd_t next_he = next_he_poly_iter->at(next_he_idx);
                hd_t cur_he = hmesh_t::null_halfedge();

                do {
                    cur_he = next_he;
                    MCUT_ASSERT(cur_he != hmesh_t::null_halfedge());
                    next_he = hmesh_t::null_halfedge();

                    cur_he_idx = next_he_idx;
                    MCUT_ASSERT(cur_he_idx != -1);
                    next_he_idx = -1;

                    cur_he_poly_iter = next_he_poly_iter;
                    MCUT_ASSERT(cur_he_poly_iter != m0_polygons.cend());
                    next_he_poly_iter = m0_polygons.cend();

                    // the next halfedge descriptor itself
                    // const hd_t& cur_he = SAFE_ACCESS(current_cs_border_he_poly, cur_he_idx); // in the polygon of current_cs_border_he

                    // get the opposite of the next halfedge in order to enter the neighbouring
                    // polygon which has a border halfedge
                    const hd_t opp_of_cur_he = m0.opposite(cur_he);

                    bool opp_of_cur_he_is_border = SAFE_ACCESS(m0_h_to_ply, opp_of_cur_he).size() == 0; // m0_h_to_ply.find(opp_of_cur_he) == m0_h_to_ply.cend(); // opposite is used to traced a polygon

                    if (opp_of_cur_he_is_border) { // found!
                        next_cs_border_he = cur_he;
                        next_cs_border_he_idx = cur_he_idx;
                        next_cs_border_he_poly_find_iter = cur_he_poly_iter;
                    } else {

                        // get index of this neighouring polygon
                        MCUT_ASSERT(SAFE_ACCESS(m0_h_to_ply, opp_of_cur_he).size() > 0 /*m0_h_to_ply.find(opp_of_cur_he) != m0_h_to_ply.cend()*/);
                        const int& opp_of_cur_he_poly_idx = SAFE_ACCESS(m0_h_to_ply, opp_of_cur_he).front(); // NOTE: class-2 or class-1 ihalfedges are incident to only one polygon
                        // reference to the neighbour/adjacent polygon
                        std::vector<traced_polygon_t>::const_iterator opp_of_cur_he_poly_iter = m0_polygons.cbegin() + (opp_of_cur_he_poly_idx);

                        MCUT_ASSERT(opp_of_cur_he_poly_iter != m0_polygons.cend());

                        // get the neighbouring polygon itself
                        const traced_polygon_t& opp_of_cur_he_poly = *opp_of_cur_he_poly_iter;
                        // get the reference to the next of opposite of current halfedge
                        const traced_polygon_t::const_iterator opp_of_cur_he_find_iter = std::find(
                            opp_of_cur_he_poly.cbegin(), opp_of_cur_he_poly.cend(), opp_of_cur_he);

                        MCUT_ASSERT(opp_of_cur_he_find_iter != opp_of_cur_he_poly.cend());

                        // index of "next of opposite of current halfedge" in the neighbour polygon
                        const int opp_of_cur_he_idx = (int)std::distance(opp_of_cur_he_poly.cbegin(), opp_of_cur_he_find_iter);
                        // get the next halfedge, which will be on the border
                        const int next_of_opp_of_cur_he_idx = wrap_integer(opp_of_cur_he_idx + 1, 0, (int)opp_of_cur_he_poly.size() - 1);

                        MCUT_ASSERT(next_of_opp_of_cur_he_idx < (int)opp_of_cur_he_poly.size());
                        const hd_t& next_of_opp_of_cur_he = SAFE_ACCESS(opp_of_cur_he_poly, next_of_opp_of_cur_he_idx); // in the polygon of opp_of_next_he_poly
                        const hd_t opp_of_next_of_opp_of_cur_he = m0.opposite(next_of_opp_of_cur_he);
                        bool opp_of_next_of_opp_of_cur_he_is_border = SAFE_ACCESS(m0_h_to_ply, opp_of_next_of_opp_of_cur_he).size() == 0; // m0_h_to_ply.find(opp_of_next_of_opp_of_cur_he) == m0_h_to_ply.cend(); // opposite is used to traced a polygon

                        // bool opp_of_next_of_opp_of_cur_he_is_border = m0_h_to_ply.find(opp_of_next_of_opp_of_cur_he) == m0_h_to_ply.cend(); // opposite is used to traced a polygon

                        if (opp_of_next_of_opp_of_cur_he_is_border) { // found!
                            next_cs_border_he = next_of_opp_of_cur_he;
                            next_cs_border_he_idx = next_of_opp_of_cur_he_idx;
                            next_cs_border_he_poly_find_iter = opp_of_cur_he_poly_iter;
                        } // else if (opp_of_next_of_opp_of_cur_he_is_border) {

                        // this is an edge-case:
                        // simple partial-cut intersection where an edge (of the cut-mesh) intersects two faces of the src-mesh

                        // get the target of the halfedge along the border of the adjacent polygon
                        //    const vd_t next_of_opp_of_cur_he_tgt = m0.target(next_of_opp_of_cur_he);

                        //     MCUT_ASSERT(m0_is_intersection_point(next_of_opp_of_cur_he_tgt, ps_vtx_cnt));

                        //    break;
                        //}
                        else {

                            const int& poly_idx = SAFE_ACCESS(m0_h_to_ply, opp_of_next_of_opp_of_cur_he).front(); // NOTE: class-2 or class-1 ihalfedges are incident to only one polygon
                            //
                            next_he_poly_iter = m0_polygons.cbegin() + (poly_idx);
                            MCUT_ASSERT(next_he_poly_iter != m0_polygons.cend());
                            const traced_polygon_t& poly = *next_he_poly_iter;
                            const traced_polygon_t::const_iterator he_find_iter = std::find(poly.cbegin(), poly.cend(), opp_of_next_of_opp_of_cur_he);
                            MCUT_ASSERT(he_find_iter != poly.cend());
                            const int idx = (int)std::distance(poly.cbegin(), he_find_iter);
                            // .. need to start from next-after, otherwise we end up in an infinite loop!
                            // see top of current do-while loop: i.e. this -> "opp_of_cur_he = m0.opposite(cur_he);" would
                            // bring us back into the current polygon
                            next_he_idx = wrap_integer(idx + 1, 0, (int)poly.size() - 1);
                            MCUT_ASSERT(next_he_idx < (int)poly.size());
                            // set next
                            // --------
                            next_he = poly[next_he_idx];
                        }
                    }

                    // while the next border halfedge has not been found OR
                    // if the next border he which is found is not actually equal to the current he
                } while (next_cs_border_he == hmesh_t::null_halfedge());
            } // while (...)

        } while (!reentrant_ivertex_queue.empty());
    } // if (partial_cut_detected) {

    cm_border_reentrant_ivtx_list.clear(); // free

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // Infer patch location (inside/outside) based on graph coloring
    ///////////////////////////////////////////////////////////////////////////

    //
    // Here we will now explicitly association location information to the graph
    // color. Thus for each color 'A', or 'B' we find out if it means "interior" or
    // "exterior" - in the geometric sense. Bare in mind that till this point the
    // notion graph node color was only used to classify patches into two sets.
    // The intuition behind this classification was that "there is two types" of
    // cut-mesh patches, 1) those which are used to seal the interior of the source-mesh
    // and 2) those used to seal the exterior". So here, we find out whether 'A' means
    // "interior" or "exterior", and vice versa for 'B'.
    //
    // DETAIL: Relying on topological alone to infer patch location is insufficient to
    // determine whether a patch lies inside or outside the source-mesh (with the exception
    // of floating patches). There is ambiguity which prevents us from knowing exactly
    // what location each color 'A' or 'B' pertains to.
    //

    TIMESTACK_PUSH("Infer patch color to location");

    std::map<char, cm_patch_location_t> patch_color_label_to_location;

    // if no exterior cut-mesh polygons where found using re-entrant vertices
    if (known_exterior_cm_polygons.empty()) {
        //
        // entering this scope means that we have a floating patch.
        // Failing to find any exterior patch(es) occurs only due the fact the
        // the cut-mesh intersectioned the src-mesh, but no edge in the cut-mesh
        // intersected a face of the source-mesh
        //
        // MCUT_ASSERT(patches.size() == 1);

        // What we will do then is assign cut-mesh polygons a value of
        // cm_patch_location_t::INSIDE since floating patches
        // are always interior

        const int patch_idx = patches.cbegin()->first;

        MCUT_ASSERT(patches.cbegin()->second.size() == 1); // should only be one polygon due to fact of having floating patch

        // find the colored entry containing the patch
        std::map<char, std::vector<int>>::const_iterator color_to_ccw_patches_find_iter = std::find_if(
            color_to_patch.cbegin(),
            color_to_patch.cend(),
            [&](const std::pair<char, std::vector<int>>& e) {
                return std::find(e.second.cbegin(), e.second.cend(), patch_idx) != e.second.cend();
            });

        // all patches must be associated with a patch by this point
        MCUT_ASSERT(color_to_ccw_patches_find_iter != color_to_patch.cend());

        //
        // We have successively inferred that the color label ('A' or 'B')
        // associated with the floating patch corresponds to "interior"
        //
        const char color_label = color_to_ccw_patches_find_iter->first;
        patch_color_label_to_location.insert(std::make_pair(color_label, cm_patch_location_t::INSIDE));

        // So, given that we know what the other color label is, we can infer
        // the location associaed with the remaining color
        patch_color_label_to_location.insert(std::make_pair(color_label == 'A' ? 'B' : 'A', cm_patch_location_t::OUTSIDE));
    } else {
        //
        // Here, we know the at least one polygon which lies on the exterior of the
        // source-mesh.
        // So lets find the patch which contains any such polygon and label this patch
        // as being "exterior".
        //

#if 1
        // std::vector<std::pair<int /*poly*/, int /*he idx*/>> known_exterior_cm_polygons;
        const std::unordered_map<int /*poly*/, int /*he idx*/>::const_iterator known_exterior_cm_polygon = known_exterior_cm_polygons.cbegin();
        MCUT_ASSERT(m0_cm_poly_to_patch_idx.find(known_exterior_cm_polygon->first) != m0_cm_poly_to_patch_idx.cend());

        // get the patch containing the polygon
        const int patch_idx = SAFE_ACCESS(m0_cm_poly_to_patch_idx, known_exterior_cm_polygon->first);

        // get the color of the patch
        std::map<char, std::vector<int>>::const_iterator color_to_ccw_patches_find_iter = std::find_if(
            color_to_patch.cbegin(),
            color_to_patch.cend(),
            [&](const std::pair<char, std::vector<int>>& e) {
                return std::find(e.second.cbegin(), e.second.cend(), patch_idx) != e.second.cend();
            });

        MCUT_ASSERT(color_to_ccw_patches_find_iter != color_to_patch.cend());

        // thus, the color of the patch means it is an "exterior" patch because it contains an exterior
        // polygon
        const char color_label = color_to_ccw_patches_find_iter->first;
        patch_color_label_to_location.insert(std::make_pair(color_label, cm_patch_location_t::OUTSIDE));

        // infer the opposite color label's meaning
        patch_color_label_to_location.insert(std::make_pair(color_label == 'A' ? 'B' : 'A', cm_patch_location_t::INSIDE));
#else
        // for each cut-mesh polygon
        for (std::map<int, int>::const_iterator cs_poly_to_patch_idx_iter = m0_cm_poly_to_patch_idx.cbegin();
             cs_poly_to_patch_idx_iter != m0_cm_poly_to_patch_idx.cend();
             ++cs_poly_to_patch_idx_iter) {

            // get index of polygon
            const int cs_poly_idx = cs_poly_to_patch_idx_iter->first;

            // check if polygon is an exterior cut-mesh polygon
            const std::vector<std::pair<int, int>>::const_iterator known_exterior_cs_polygons_find_iter = std::find_if(
                known_exterior_cm_polygons.cbegin(),
                known_exterior_cm_polygons.cend(),
                [&](const std::pair<int, int>& e) { return e.first == cs_poly_idx; });
            const bool poly_is_known_exterior_cs_polygon = (known_exterior_cs_polygons_find_iter != known_exterior_cm_polygons.cend());

            if (poly_is_known_exterior_cs_polygon) {
                // get the patch containing the polygon
                const int patch_idx = cs_poly_to_patch_idx_iter->second;

                // get the color of the patch
                std::map<char, std::vector<int>>::const_iterator color_to_ccw_patches_find_iter = std::find_if(
                    color_to_patch.cbegin(),
                    color_to_patch.cend(),
                    [&](const std::pair<char, std::vector<int>>& e) {
                        return std::find(e.second.cbegin(), e.second.cend(), patch_idx) != e.second.cend();
                    });

                MCUT_ASSERT(color_to_ccw_patches_find_iter != color_to_patch.cend());

                // thus, the color of the patch means it is an "exterior" patch because it contains an exterior
                // polygon
                const char color_label = color_to_ccw_patches_find_iter->first;
                patch_color_label_to_location.insert(std::make_pair(color_label, cm_patch_location_t::OUTSIDE));

                // infer the opposite color label's meaning
                patch_color_label_to_location.insert(std::make_pair(color_label == 'A' ? 'B' : 'A', cm_patch_location_t::INSIDE));

                break; // done (only need to find the first exterior polygon for use to know everything else)
            }
        }

#endif
    }
    MCUT_ASSERT(!patch_color_label_to_location.empty());

    known_exterior_cm_polygons.clear();
    // m0_cm_poly_to_patch_idx.clear();
#if 0
        // dump
        
        
        for (std::map<char, std::vector<int>>::const_iterator color_to_ccw_patches_iter = color_to_patch.cbegin(); color_to_ccw_patches_iter != color_to_patch.cend(); ++color_to_ccw_patches_iter)
        {
            const char color_label = color_to_ccw_patches_iter->first;
            //const cm_patch_location_t color_label_dye = SAFE_ACCESS(patch_color_label_to_location, color_label);

            
        }

#endif
    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // Create reverse patches
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Create reversed patches");

    const int traced_polygon_count = (int)m0_polygons.size(); // does not include the reversed cut-mesh polygons

    // note: reversed patches are called "cw" patches (for assumed clockwise based on input meshes)

    std::map<
        char, // color tag
        std::vector<int> // reversed patch index
        >
        color_to_cw_patch;

    std::map<
        int, // patch index
        int // opposite patch index
        >
        patch_to_opposite;

    // for each color
    for (std::map<char, std::vector<int>>::const_iterator color_to_ccw_patches_iter = color_to_patch.cbegin();
         color_to_ccw_patches_iter != color_to_patch.cend();
         ++color_to_ccw_patches_iter) {

        // const char color_id = color_to_ccw_patches_iter->first;

        //

        // add entry
        MCUT_ASSERT(color_to_cw_patch.count(color_to_ccw_patches_iter->first) == 0);
        color_to_cw_patch[color_to_ccw_patches_iter->first] = std::vector<int>();
        // std::pair<std::map<char, std::vector<int>>::iterator, bool> color_to_cw_patch_insertion = color_to_cw_patch.insert(std::make_pair(color_to_ccw_patches_iter->first, std::vector<int>()));
        // MCUT_ASSERT(color_to_cw_patch_insertion.second == true);
        MCUT_ASSERT(color_to_cw_patch.count(color_to_ccw_patches_iter->first) == 1);

        // list of reversed patches with current color
        std::vector<int>& cw_patch_color = SAFE_ACCESS(color_to_cw_patch, color_to_ccw_patches_iter->first);

        // for each patch with current color
        for (std::vector<int>::const_iterator patch_iter = color_to_ccw_patches_iter->second.cbegin();
             patch_iter != color_to_ccw_patches_iter->second.cend();
             ++patch_iter) {

            const int patch_idx = *patch_iter;

            const std::vector<int>& patch = SAFE_ACCESS(patches, patch_idx);

            //
            // create reversed patch
            //

            const int cw_patch_idx = (int)patches.size(); // index of reversed version

            // relate patch to opposite
            patch_to_opposite[patch_idx] = cw_patch_idx;
            patch_to_opposite[cw_patch_idx] = patch_idx;

            MCUT_ASSERT(patches.count(cw_patch_idx) == 0);
            patches[cw_patch_idx] = std::vector<int>();
            // std::pair<std::map<int, std::vector<int>>::iterator, bool> patch_insertion = patches.insert(std::make_pair(cw_patch_idx, std::vector<int>()));
            // MCUT_ASSERT(patch_insertion.second == true);
            MCUT_ASSERT(patches.count(cw_patch_idx) == 1);

            std::vector<int>& cw_patch = SAFE_ACCESS(patches, cw_patch_idx);

            // add to list of patches with current color
            cw_patch_color.push_back(cw_patch_idx);

            /*
            for each polygon in patch
                if is floating-patch polygon
                    find the opposite polygon (which already exists)
                    add opposite polygon into patch
                else
                    create reversed version and update data structures
        */

            // number of polygons in the ccw patch
            const int initial_patch_size = (int)patch.size();

            // for each polygon in the ccw patch
            for (int ccw_patch_iter = 0; ccw_patch_iter < initial_patch_size; ++ccw_patch_iter) {

                // get the polygon index
                const int ccw_patch_poly_idx = SAFE_ACCESS(patch, ccw_patch_iter);

                // all polygon are stored in the same array so we can use that to deduce
                // index of new reversed polygon
                int cw_poly_idx = (int)m0_polygons.size();

                // get the normal polygon
                const traced_polygon_t& patch_poly = SAFE_ACCESS(m0_polygons, ccw_patch_poly_idx);
#if 0
                    const bool is_floating_patch = SAFE_ACCESS(patch_to_floating_flag, patch_idx);

                    if (is_floating_patch)
                    {
                        //
                        // the reversed polygon of the current normal polygon already exists.
                        // So we can just use that. Note: this is because the polygon tracing
                        // that we did earlier (in "m0") always traces two versions of a polygon
                        // whose halfedges are all interior intersection-halfedges (x-->x)
                        //

                        // find opposite polygon
                        const hd_t &coincident_halfedge = patch_poly.front(); // can be any
                        const hd_t coincident_halfedge_opp = m0.opposite(coincident_halfedge);
                        const std::vector<int> &coincident_polys = SAFE_ACCESS(m0_h_to_ply, coincident_halfedge_opp);
                        // find coincident cut-mesh polygon
                        const std::vector<int>::const_iterator coincident_polys_find_iter = std::find_if(
                            coincident_polys.cbegin(),
                            coincident_polys.cend(),
                            [&](const int &e)
                            { return e >= traced_sm_polygon_count && e < traced_polygon_count; });

                        MCUT_ASSERT(coincident_polys_find_iter != coincident_polys.cend());

                        const int patch_poly_opp = *coincident_polys_find_iter;
                        cw_poly_idx = patch_poly_opp; // found opposite polygon
                    }
                    else
#endif
                {

                    //
                    // the current ccw polygon does not form a floating patch,
                    // so we calculate the reversed version by retracing the
                    // connectivity in reverse order
                    //

                    traced_polygon_t cw_poly;
                    cw_poly.reserve(patch_poly.size());
                    traced_polygon_t tmp;
                    tmp.reserve(patch_poly.size());

                    // for each halfedge of the ccw polygon
                    for (traced_polygon_t::const_iterator patch_poly_he_iter = patch_poly.cbegin();
                         patch_poly_he_iter != patch_poly.cend();
                         ++patch_poly_he_iter) {

                        // get halfedge descriptor
                        const hd_t& patch_poly_he = *patch_poly_he_iter;
                        // get the opposite halfedge
                        const hd_t patch_poly_he_opp = m0.opposite(patch_poly_he);
                        // add into list defining reversed polygon
                        tmp.push_back(patch_poly_he_opp);
                        // check if another cut-mesh polygon is traced with this opposite halfedge.
                        std::vector<std::vector<int>>::iterator find_iter = m0_h_to_ply.begin() + patch_poly_he_opp; // m0_h_to_ply.find(patch_poly_he_opp);
#if 0
                            if (find_iter == m0_h_to_ply.end())
                            { // "patch_poly_he_opp" not used to trace any polygon
                                //
                                // we only enter this scope of the halfedge "patch_poly_he_opp" is a
                                // border halfedge which is not used to trace a cut-mesh polygon.

                                // add entry for the halfedge
                                std::pair<std::map<hd_t, std::vector<int>>::iterator, bool> m0_he_to_poly_idx_insertion = m0_h_to_ply.insert(std::make_pair(patch_poly_he_opp, std::vector<int>()));

                                MCUT_ASSERT(m0_he_to_poly_idx_insertion.second == true);

                                find_iter = m0_he_to_poly_idx_insertion.first;
                            }
#endif
                        // associate "patch_poly_he_opp" with the new reversed polygon
                        find_iter->push_back(cw_poly_idx);
                    }

                    MCUT_ASSERT(tmp.size() == patch_poly.size());

                    // reverse the order to ensure correct winding, last halfedge for goes to beginning, and so on...
                    for (int h = 0; h < (int)tmp.size(); ++h) {
                        const int index = (int)tmp.size() - 1 - h;
                        cw_poly.push_back(SAFE_ACCESS(tmp, index));
                    }

                    //{
                    //    for (traced_polygon_t::const_iterator cw_poly_he_iter = cw_poly.cbegin(); cw_poly_he_iter != cw_poly.cend(); ++cw_poly_he_iter) {
                    ///    }
                    //}

                    MCUT_ASSERT(m0.source(cw_poly.front()) == m0.target(cw_poly.back())); // must form loop

                    m0_polygons.push_back(cw_poly); // save the new polygon!
                }

                // the new polygon's index as being part of the patch
                cw_patch.push_back(cw_poly_idx);

                // map the reversed polygon to its patch
                MCUT_ASSERT(m0_cm_poly_to_patch_idx.count(cw_poly_idx) == 0);
                m0_cm_poly_to_patch_idx[cw_poly_idx] = cw_patch_idx;

                // map the reversed polygon to the same ps-face as its ccw counterpart!
                if (m0_to_ps_face.count(cw_poly_idx) == 0) {
                    // must not be a floating patch because such ccw and cw patch polygons are created and mapped during "m0" tracing stage
                    // MCUT_ASSERT(patch_to_floating_flag.count(cw_poly_idx) == false);
                    MCUT_ASSERT(m0_to_ps_face.count(ccw_patch_poly_idx) == 1);
                    // both polygon will have originated from the same ps-face!
                    m0_to_ps_face[cw_poly_idx] = SAFE_ACCESS(m0_to_ps_face, ccw_patch_poly_idx);
                }
            }
        }
    }

    TIMESTACK_POP();

    // number of reversed cut-mesh polygons
    // const int cw_cs_poly_count = ((int)m0_polygons.size() - traced_polygon_count);

    //

    // NOTE: at this stage, all patch polygons (ccw/normal) also have an opposite (cw/reversed)

    // merge the opposite color_to_patch data structure

    TIMESTACK_PUSH("merge opposite color to path data structures");

    // for each color
    for (std::map<char, std::vector<int>>::const_iterator color_to_cw_patch_iter = color_to_cw_patch.cbegin();
         color_to_cw_patch_iter != color_to_cw_patch.cend();
         ++color_to_cw_patch_iter) {

        const char color_value = color_to_cw_patch_iter->first;

        // get reversed patches
        const std::vector<int>& colored_cw_patches = color_to_cw_patch_iter->second;
        // get normal patches
        std::vector<int>& colored_patches = SAFE_ACCESS(color_to_patch, color_value);
        // merge
        colored_patches.insert(colored_patches.end(), colored_cw_patches.cbegin(), colored_cw_patches.cend()); // merge

        // dump
        if (input.verbose) {

            for (std::vector<int>::const_iterator colored_patch_iter = colored_patches.cbegin();
                 colored_patch_iter != colored_patches.cend();
                 ++colored_patch_iter) {

                const int patch_idx = *colored_patch_iter;
                const std::vector<int>& patch = SAFE_ACCESS(patches, patch_idx);
                // const int is_ccw = (int)(std::distance(colored_patches.cbegin(), colored_patch_iter) < (int)(patch.size() / 2));

                //

                for (std::vector<int>::const_iterator patch_poly_iter = patch.cbegin(); patch_poly_iter != patch.cend(); ++patch_poly_iter) {
                }
            }
        }
    }

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // save the patches into the output
    ///////////////////////////////////////////////////////////////////////////

    TIMESTACK_PUSH("Save patches");

    // for each color
    for (std::map<char, std::vector<int>>::const_iterator color_to_patches_iter = color_to_patch.cbegin();
         color_to_patches_iter != color_to_patch.cend();
         ++color_to_patches_iter) {

        const char color_id = color_to_patches_iter->first;

        // for each patch with current color
        for (std::vector<int>::const_iterator patch_iter = color_to_patches_iter->second.cbegin();
             patch_iter != color_to_patches_iter->second.cend();
             ++patch_iter) {

            const int cur_patch_idx = *patch_iter;

            const cm_patch_location_t& patch_location = SAFE_ACCESS(patch_color_label_to_location, color_id);
            if ((patch_location == cm_patch_location_t::INSIDE && !input.keep_inside_patches) || //
                (patch_location == cm_patch_location_t::OUTSIDE && !input.keep_outside_patches)) {
                continue;
            }

            //
            // create mesh for patch
            //
            std::shared_ptr<hmesh_t> patch_mesh = std::shared_ptr<hmesh_t>(new hmesh_t);
            patch_mesh->reserve_for_additional_elements(cs_face_count);

            std::unordered_map<
                vd_t, // vertex descriptor in "m0"
                vd_t // vertex descriptor in "patch_mesh"
                >
                m0_to_patch_mesh_vertex;

            m0_to_patch_mesh_vertex.reserve(m0.number_of_vertices());

            // NOTE: ccw/normal patches are created before their reversed counterparts (hence the modulo Operator trick)

            // is the a normal patch
            const bool is_ccw_patch = ((cur_patch_idx % total_ccw_patch_count) == cur_patch_idx);
            const cm_patch_winding_order_t patch_descriptor = is_ccw_patch ? cm_patch_winding_order_t::DEFAULT : cm_patch_winding_order_t::REVERSE;
            const std::string cs_patch_descriptor_str = to_string(patch_descriptor);

            // get the patch's polygons
            const std::vector<int>& patch = SAFE_ACCESS(patches, cur_patch_idx);

            //
            // add vertices into patch mesh
            //

            std::vector<vd_t> seam_vertices; // vertices along cutpath

            std::unordered_map<vd_t, vd_t> patch_to_m0_vertex;
            patch_to_m0_vertex.reserve(cs_face_count);

            // for each polygon in the patch
            for (std::vector<int>::const_iterator patch_poly_iter = patch.cbegin(); patch_poly_iter != patch.cend(); ++patch_poly_iter) {

                const int& patch_poly_idx = *patch_poly_iter;
                const traced_polygon_t& patch_poly = SAFE_ACCESS(m0_polygons, patch_poly_idx);

                // for each halfedge of polygon
                for (traced_polygon_t::const_iterator patch_poly_he_iter = patch_poly.cbegin();
                     patch_poly_he_iter != patch_poly.cend();
                     ++patch_poly_he_iter) {

                    const vd_t m0_vertex = m0.target(*patch_poly_he_iter);
                    const bool vertex_already_mapped = m0_to_patch_mesh_vertex.find(m0_vertex) != m0_to_patch_mesh_vertex.cend();

                    if (!vertex_already_mapped) {
                        // map from "m0" to "patch_mesh" descriptor
                        const vd_t& patch_mesh_vertex = patch_mesh->add_vertex(m0.vertex(m0_vertex));

                        MCUT_ASSERT(patch_mesh_vertex != hmesh_t::null_halfedge());

                        m0_to_patch_mesh_vertex.insert(std::make_pair(m0_vertex, patch_mesh_vertex));
                        patch_to_m0_vertex.insert(std::make_pair(patch_mesh_vertex, m0_vertex));

                        // mark if is seam vertex
                        if (m0_is_intersection_point(m0_vertex, ps_vtx_cnt)) {
                            seam_vertices.push_back(patch_mesh_vertex); // seam vertices are intersection points
                        }
                    }
                }
            }

            MCUT_ASSERT(!seam_vertices.empty());

            //
            // add faces into patch mesh
            //

            std::unordered_map<fd_t, int> patch_to_m0_face;
            patch_to_m0_face.reserve(patch.size());

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            auto fn_remap_face_vertices = [&](std::vector<int>::const_iterator block_start_, std::vector<int>::const_iterator block_end_) {
                std::vector<std::pair<int, std::vector<vd_t>>> result;
                result.resize(std::distance(block_start_, block_end_));

                uint32_t counter = 0;
                // for each polygon
                for (std::vector<int>::const_iterator patch_poly_iter = block_start_; patch_poly_iter != block_end_; ++patch_poly_iter) {
                    std::vector<vd_t>& remapped_poly_vertices = result[counter].second; // redefined face using "patch_mesh" descriptors
                    const int& patch_poly_idx = *patch_poly_iter;
                    result[counter].first = patch_poly_idx;
                    const traced_polygon_t& patch_poly = SAFE_ACCESS(m0_polygons, patch_poly_idx);

                    remapped_poly_vertices.reserve(patch_poly.size());

                    // for each halfedge
                    for (traced_polygon_t::const_iterator patch_poly_he_iter = patch_poly.cbegin();
                         patch_poly_he_iter != patch_poly.cend();
                         ++patch_poly_he_iter) {
                        const vd_t m0_vertex = m0.target(*patch_poly_he_iter);
                        const vd_t patch_mesh_vertex = SAFE_ACCESS(m0_to_patch_mesh_vertex, m0_vertex);
                        remapped_poly_vertices.push_back(patch_mesh_vertex);
                    }

                    counter++;
                }

                return result;
            };

            std::vector<std::future<std::vector<std::pair<int, std::vector<vd_t>>>>> futures;
            std::vector<std::pair<int, std::vector<vd_t>>> partial_res;

            parallel_for(
                *input.scheduler,
                patch.cbegin(),
                patch.cend(),
                fn_remap_face_vertices,
                partial_res, // output computed by master thread
                futures);

            auto add_face_and_save_mapping = [&](const std::pair<int, std::vector<vd_t>>& remapped_poly_info) {
                const std::vector<vd_t>& remapped_poly_vertices = remapped_poly_info.second;
                const int patch_poly_idx = remapped_poly_info.first;

                const fd_t f = patch_mesh->add_face(remapped_poly_vertices);

                MCUT_ASSERT(f != hmesh_t::null_face());

                patch_to_m0_face.insert(std::make_pair(f, patch_poly_idx));
            };

            // to maintain face insertion order, we add according to the scheduling

            for (int i = 0; i < (int)futures.size(); ++i) {
                std::future<std::vector<std::pair<int, std::vector<vd_t>>>>& f = futures[i];
                MCUT_ASSERT(f.valid());
                std::vector<std::pair<int, std::vector<vd_t>>> future_result = f.get(); // "get()" is a blocking function

                // for each polygon
                for (uint32_t j = 0; j < (uint32_t)future_result.size(); ++j) {
                    const std::pair<int, std::vector<vd_t>>& remapped_poly_info = future_result[j];
                    add_face_and_save_mapping(remapped_poly_info);
                }
            }

            // for each polygon
            for (uint32_t j = 0; j < (uint32_t)partial_res.size(); ++j) {
                const std::pair<int, std::vector<vd_t>>& remapped_poly_info = partial_res[j];
                add_face_and_save_mapping(remapped_poly_info);
            }

#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            std::vector<vd_t> remapped_poly_vertices; // redefined face using "patch_mesh" descriptors
            // for each polygon
            for (std::vector<int>::const_iterator patch_poly_iter = patch.cbegin(); patch_poly_iter != patch.cend(); ++patch_poly_iter) {

                const int& patch_poly_idx = *patch_poly_iter;
                const traced_polygon_t& patch_poly = SAFE_ACCESS(m0_polygons, patch_poly_idx);

                remapped_poly_vertices.clear();
                remapped_poly_vertices.reserve(patch_poly.size());

                // for each halfedge
                for (traced_polygon_t::const_iterator patch_poly_he_iter = patch_poly.cbegin();
                     patch_poly_he_iter != patch_poly.cend();
                     ++patch_poly_he_iter) {
                    const vd_t m0_vertex = m0.target(*patch_poly_he_iter);
                    const vd_t patch_mesh_vertex = SAFE_ACCESS(m0_to_patch_mesh_vertex, m0_vertex);
                    remapped_poly_vertices.push_back(patch_mesh_vertex);
                }

                const fd_t f = patch_mesh->add_face(remapped_poly_vertices);

                MCUT_ASSERT(f != hmesh_t::null_face());

                patch_to_m0_face.insert(std::make_pair(f, patch_poly_idx));
            }

#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

            if (input.verbose) {
                dump_mesh(patch_mesh.get()[0], ("patch" + std::to_string(cur_patch_idx) + "." + to_string(patch_location) + "." + cs_patch_descriptor_str).c_str());
            }

            std::shared_ptr<output_mesh_info_t> omi = std::shared_ptr<output_mesh_info_t>(new output_mesh_info_t);
            omi->mesh = patch_mesh;
            omi->seam_vertices = std::move(seam_vertices);

            if (input.populate_vertex_maps) {
                // compute vertex mapping
                // ----------------------

                omi->data_maps.vertex_map.resize(patch_mesh->number_of_vertices());

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                {
                    auto fn_fill_vertex_map = [&](vertex_array_iterator_t block_start_, vertex_array_iterator_t block_end_) {
                        for (vertex_array_iterator_t v = block_start_; v != block_end_; ++v) {
                            MCUT_ASSERT(patch_to_m0_vertex.count(*v) == 1);
                            const vd_t as_m0_descr = SAFE_ACCESS(patch_to_m0_vertex, *v);
                            vd_t as_cm_descr = hmesh_t::null_vertex();

                            if ((int)as_m0_descr < (int)m0_to_ps_vtx.size() /*m0_to_ps_vtx.count(as_m0_descr) == 1*/) {
                                vd_t as_ps_descr = SAFE_ACCESS(patch_to_m0_vertex, *v);

                                MCUT_ASSERT((int)as_ps_descr < (int)ps_to_cm_vtx.size() /*ps_to_cm_vtx.count(as_ps_descr) == 1*/);
                                as_cm_descr = SAFE_ACCESS(ps_to_cm_vtx, as_ps_descr);

                                // add an offset which allows users to deduce which birth/origin mesh (source or cut mesh) a face (map value) belongs to.
                                as_cm_descr = static_cast<vd_t>(as_cm_descr + sm_vtx_cnt);
                            }

                            MCUT_ASSERT(SAFE_ACCESS(omi->data_maps.vertex_map, *v) == hmesh_t::null_vertex() /*omi->data_maps.vertex_map.count(*v) == 0*/);
                            omi->data_maps.vertex_map[*v] = as_cm_descr;
                        }
                    };

                    parallel_for(
                        *input.scheduler,
                        patch_mesh->vertices_begin(),
                        patch_mesh->vertices_end(),
                        fn_fill_vertex_map);
                }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                for (vertex_array_iterator_t v = patch_mesh->vertices_begin(); v != patch_mesh->vertices_end(); ++v) {
                    MCUT_ASSERT(patch_to_m0_vertex.count(*v) == 1);
                    const vd_t as_m0_descr = SAFE_ACCESS(patch_to_m0_vertex, *v);
                    vd_t as_cm_descr = hmesh_t::null_vertex();

                    if ((int)as_m0_descr < (int)m0_to_ps_vtx.size() /*m0_to_ps_vtx.count(as_m0_descr) == 1*/) {
                        vd_t as_ps_descr = SAFE_ACCESS(patch_to_m0_vertex, *v);

                        MCUT_ASSERT((int)as_ps_descr < (int)ps_to_cm_vtx.size() /*ps_to_cm_vtx.count(as_ps_descr) == 1*/);
                        as_cm_descr = SAFE_ACCESS(ps_to_cm_vtx, as_ps_descr);

                        // add an offset which allows users to deduce which birth/origin mesh (source or cut mesh) a face (map value) belongs to.
                        as_cm_descr = static_cast<vd_t>(as_cm_descr + sm_vtx_cnt);
                    }

                    MCUT_ASSERT(SAFE_ACCESS(omi->data_maps.vertex_map, *v) == hmesh_t::null_vertex() /*omi->data_maps.vertex_map.count(*v) == 0*/);
                    omi->data_maps.vertex_map[*v] = as_cm_descr;
                }
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            }

            if (input.populate_face_maps) {
                // compute face mapping
                // ----------------------

                omi->data_maps.face_map.resize(patch_mesh->number_of_faces());
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                {
                    auto fn_fill_face_map = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
                        for (face_array_iterator_t f = block_start_; f != block_end_; ++f) {
                            MCUT_ASSERT(patch_to_m0_face.count(*f) == 1);
                            const int as_m0_descr = SAFE_ACCESS(patch_to_m0_face, *f);

                            MCUT_ASSERT(m0_to_ps_face.count(as_m0_descr) == 1);
                            const fd_t as_ps_descr = SAFE_ACCESS(m0_to_ps_face, as_m0_descr);

                            MCUT_ASSERT((int)as_ps_descr < (int)ps_to_cm_face.size() /*ps_to_cm_face.count(as_ps_descr) == 1*/);
                            fd_t as_cm_descr = SAFE_ACCESS(ps_to_cm_face, as_ps_descr);

                            // add an offset which allows users to deduce which birth/origin mesh (source or cut mesh) a face (map value) belongs to.
                            as_cm_descr = static_cast<fd_t>(as_cm_descr + sm_face_count);

                            MCUT_ASSERT(SAFE_ACCESS(omi->data_maps.face_map, *f) == hmesh_t::null_face() /*omi->data_maps.face_map.count(*f) == 0*/);
                            omi->data_maps.face_map[*f] = as_cm_descr;
                        }
                    };

                    parallel_for(
                        *input.scheduler,
                        patch_mesh->faces_begin(),
                        patch_mesh->faces_end(),
                        fn_fill_face_map);
                }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                for (face_array_iterator_t f = patch_mesh->faces_begin(); f != patch_mesh->faces_end(); ++f) {
                    MCUT_ASSERT(patch_to_m0_face.count(*f) == 1);
                    const int as_m0_descr = SAFE_ACCESS(patch_to_m0_face, *f);

                    MCUT_ASSERT(m0_to_ps_face.count(as_m0_descr) == 1);
                    const fd_t as_ps_descr = SAFE_ACCESS(m0_to_ps_face, as_m0_descr);

                    MCUT_ASSERT((int)as_ps_descr < (int)ps_to_cm_face.size() /*ps_to_cm_face.count(as_ps_descr) == 1*/);
                    fd_t as_cm_descr = SAFE_ACCESS(ps_to_cm_face, as_ps_descr);

                    // add an offset which allows users to deduce which birth/origin mesh (source or cut mesh) a face (map value) belongs to.
                    as_cm_descr = static_cast<fd_t>(as_cm_descr + sm_face_count);

                    MCUT_ASSERT(SAFE_ACCESS(omi->data_maps.face_map, *f) == hmesh_t::null_face() /*omi->data_maps.face_map.count(*f) == 0*/);
                    omi->data_maps.face_map[*f] = as_cm_descr;
                }
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            }

            if (patch_location == cm_patch_location_t::INSIDE) {
                output.inside_patches[patch_descriptor].emplace_back((omi));
            } else {
                output.outside_patches[patch_descriptor].emplace_back((omi));
            }
        }
    }

    TIMESTACK_POP();

    if (false == (input.keep_fragments_below_cutmesh || //
            input.keep_fragments_above_cutmesh || //
            input.keep_fragments_partially_cut || //
            input.keep_fragments_sealed_inside || //
            input.keep_fragments_sealed_outside || input.keep_fragments_sealed_inside_exhaustive || //
            input.keep_fragments_sealed_outside_exhaustive)) {
        // if the user simply wants [patches], then we should not have to proceed further.
        return;
    }

    ///////////////////////////////////////////////////////////////////////////
    // calculate the reversed patch seeds
    ///////////////////////////////////////////////////////////////////////////

    //
    // Here, we will infer the seed [interior intersection-halfedges] and polygons
    // for the newly create reversed polygons. We will also save information telling
    // whether each reversed patch is a floating patch or not. We will use this
    // information during stitching
    //

    TIMESTACK_PUSH("Create reversed-patch seed variables");

    // for each color
    for (std::map<char, std::vector<int>>::const_iterator color_to_cw_patch_iter = color_to_cw_patch.cbegin();
         color_to_cw_patch_iter != color_to_cw_patch.cend();
         ++color_to_cw_patch_iter) {

        // const char color_value = color_to_cw_patch_iter->first;

        //

        // get the reversed patch of the current color
        const std::vector<int>& colored_cw_patches = color_to_cw_patch_iter->second;

        // for each patch
        for (std::vector<int>::const_iterator colored_cw_patch_iter = colored_cw_patches.cbegin();
             colored_cw_patch_iter != colored_cw_patches.cend();
             ++colored_cw_patch_iter) {

            const int cw_patch_idx = *colored_cw_patch_iter;

            // get patch polygons
            // const std::vector<int>& cw_patch = SAFE_ACCESS(patches, cw_patch_idx);
            // get the opposite patch
            const int ccw_patch_idx = SAFE_ACCESS(patch_to_opposite, cw_patch_idx); // opposite patch

            //
            // copy information from opposite (ccw/normal) patch
            //
            // std::pair<std::map<int, bool>::const_iterator, bool> patch_to_floating_flag_insertion = patch_to_floating_flag.insert(
            //    std::make_pair(cw_patch_idx, SAFE_ACCESS(patch_to_floating_flag, ccw_patch_idx)));

            // MCUT_ASSERT(patch_to_floating_flag_insertion.second == true);

            // was the opposite patch determined to be a floating patch
            // const bool is_floating_patch = patch_to_floating_flag_insertion.first->second;
            // get the index of seed interior intersection halfedge of the opposite ccw/normal patch
            const int ccw_patch_seed_interior_ihalfedge_idx = SAFE_ACCESS(patch_to_seed_interior_ihalfedge_idx, ccw_patch_idx);
            // get the index of seed polygon of the opposite ccw/normal patch
            const int ccw_patch_seed_poly_idx = SAFE_ACCESS(patch_to_seed_poly_idx, ccw_patch_idx);
            // get the seed polygon of the opposite ccw/normal patch
            const traced_polygon_t& ccw_patch_seed_poly = SAFE_ACCESS(m0_polygons, ccw_patch_seed_poly_idx);
            // get the seed interior intersection halfedge of the opposite ccw/normal patch
            const hd_t& ccw_patch_seed_interior_ihalfedge = SAFE_ACCESS(ccw_patch_seed_poly, ccw_patch_seed_interior_ihalfedge_idx);
            // opposite halfedge of the seed interior intersection halfedge of the opposite ccw/normal patch
            const hd_t ccw_patch_seed_interior_ihalfedge_opp = m0.opposite(ccw_patch_seed_interior_ihalfedge);

            // find the reversed polygon which uses "ccw_patch_seed_interior_ihalfedge_opp"
            // this will be the seed polygon of the current reversed patch
            const std::vector<int>& coincident_polys = SAFE_ACCESS(m0_h_to_ply, ccw_patch_seed_interior_ihalfedge_opp);

            std::vector<int>::const_iterator find_iter = std::find_if(coincident_polys.cbegin(), coincident_polys.cend(),
                [&](const int& e) {
#if 0
                                                                              if (is_floating_patch)
                                                                              {
                                                                                  return e >= traced_sm_polygon_count && e < traced_polygon_count; // interior ihalfedges of floating patches are already coincident to two polygons due to polygon tracing
                                                                              }
                                                                              else
#endif
                    {
                        return e >= traced_polygon_count;
                    }
                });

            MCUT_ASSERT(find_iter != coincident_polys.cend());

            // the index of the seed polygon of the current reversed patch
            const int cw_patch_seed_poly_idx = *find_iter;

            // the patch must contain the polygon
            MCUT_ASSERT(std::find(SAFE_ACCESS(patches, cw_patch_idx).cbegin(), SAFE_ACCESS(patches, cw_patch_idx).cend(), cw_patch_seed_poly_idx) != SAFE_ACCESS(patches, cw_patch_idx).cend());

            const traced_polygon_t& cw_patch_seed_poly = SAFE_ACCESS(m0_polygons, cw_patch_seed_poly_idx);
            traced_polygon_t::const_iterator he_find_iter = std::find(cw_patch_seed_poly.cbegin(), cw_patch_seed_poly.cend(), ccw_patch_seed_interior_ihalfedge_opp);

            MCUT_ASSERT(he_find_iter != cw_patch_seed_poly.cend());

            // the index of the interior intersection halfedge of the current reversed patch
            const int opposite_patch_seed_interior_ihalfedge_idx = (int)std::distance(cw_patch_seed_poly.cbegin(), he_find_iter);

            MCUT_ASSERT(patch_to_seed_interior_ihalfedge_idx.count(cw_patch_idx) == 0);
            patch_to_seed_interior_ihalfedge_idx[cw_patch_idx] = opposite_patch_seed_interior_ihalfedge_idx;
            // std::pair<std::map<int, int>::const_iterator, bool> seed_interior_ihalfedge_idx_insertion = patch_to_seed_interior_ihalfedge_idx.insert(std::make_pair(cw_patch_idx, opposite_patch_seed_interior_ihalfedge_idx));
            //  MCUT_ASSERT(seed_interior_ihalfedge_idx_insertion.second == true);
            MCUT_ASSERT(patch_to_seed_interior_ihalfedge_idx.count(cw_patch_idx) == 1);

            MCUT_ASSERT(patch_to_seed_poly_idx.count(cw_patch_idx) == 0);
            patch_to_seed_poly_idx[cw_patch_idx] = cw_patch_seed_poly_idx;
            // std::pair<std::map<int, int>::const_iterator, bool> seed_poly_idx_insertion = patch_to_seed_poly_idx.insert(std::make_pair(cw_patch_idx, cw_patch_seed_poly_idx));
            // MCUT_ASSERT(seed_poly_idx_insertion.second == true);
            MCUT_ASSERT(patch_to_seed_poly_idx.count(cw_patch_idx) == 1);
        }
    }

    // patch_to_floating_flag.clear(); // free
    color_to_cw_patch.clear(); // free
    patch_to_opposite.clear(); // free

    TIMESTACK_POP();

    ///////////////////////////////////////////////////////////////////////////
    // Stitch cut-mesh patches into connected components (fragments) of the source-mesh
    ///////////////////////////////////////////////////////////////////////////

    //
    // We are now going to "fill the holes"
    //
    // For each color tag, we have a halfdge data structure (which is a copy of "m1").
    // We do this to make sure that the exterior/outside patches will be stitched to
    // separate copies of the connected components in "m1", compared to interior patches.
    // This helps us to distinguish between stitching the interior of the source-mesh
    // (hole-filling), and stitching the exterior (i.e. boolean merge operation
    // if the cut-mesh is water-tight)
    //

    TIMESTACK_PUSH("Stitching"); // &&&&&

    std::map<
        char, // color tag
        std::map<
            std::size_t, // cc-id
            std::vector< // list of partially sealed connected components (first elem has 1 stitched polygon and the last has all cut-mesh polygons stitched to fill holes)
                std::pair< // mesh instance
                    std::shared_ptr<hmesh_t>, // actual mesh data structure
                    connected_component_info_t // information about mesh
                    >>>>
        color_to_separated_connected_ccsponents;

    std::map<
        char, // color tag
        std::vector<traced_polygon_t> // traced polygons in colored "m1" mesh
        >
        color_to_m1_polygons;

    // A halfedge in "m0" that is used to trace a cut-mesh polygon will have
    // two "m1" versions - one for the ccw/normal patch and the other for the
    // cw/reversed patch.
    //
    std::map<
        char, // color tag
        std::unordered_map<
            hd_t, // "m0" cut-mesh halfedge instance
            std::map<
                int, // patch idx
                hd_t // "m1" cut-mesh halfedge instance
                >>>
        color_to_m0_to_m1_he_instances;

    std::map<
        char, // color value
        std::unordered_map<int, int> // copy of m0_to_m1_face (initially containing mappings just for traced source-mesh polygon)
        >
        color_to_m0_to_m1_face; //  = { { 'A', m0_to_m1_face }, { 'B', m0_to_m1_face } };

    color_to_m0_to_m1_face.emplace(std::make_pair('A', m0_to_m1_face));
    color_to_m0_to_m1_face.emplace(std::make_pair('B', std::move(m0_to_m1_face)));
    // m0_to_m1_face.clear();

    std::map<
        char, // color value
        std::unordered_map<int, int> // copy of m1_to_m0_face (initially containing mappings just for traced source-mesh polygon)
        >
        color_to_m1_to_m0_face; // = { { 'A', m1_to_m0_face }, { 'B', m1_to_m0_face } };
    color_to_m1_to_m0_face.emplace(std::make_pair('A', m1_to_m0_face));
    color_to_m1_to_m0_face.emplace(std::make_pair('B', std::move(m1_to_m0_face)));
    // m1_to_m0_face.clear();

    std::map<char, std::unordered_map<vd_t, // "m1" cut-mesh vtx instance
                       vd_t // "m0" cut-mesh ovtx instance
                       >>
        colour_to_m1_to_m0_cm_ovtx;

    std::map<
        char, // color value
        std::vector<vd_t> // copy of "m0_to_m1_ovtx" (initially containing mappings just for original source-mesh & cut-mesh vertices i.e. no ivertices included!)
        >
        color_to_m1_to_m0_sm_ovtx; // = { { 'A', m1_to_m0_ovtx }, { 'B', m1_to_m0_ovtx } };
    color_to_m1_to_m0_sm_ovtx.emplace(std::make_pair('A', m1_to_m0_ovtx));
    color_to_m1_to_m0_sm_ovtx.emplace(std::make_pair('B', std::move(m1_to_m0_ovtx)));
    // m1_to_m0_ovtx.clear();

    // this queue contains information identifying the patch polygons next-in-queue
    // to be stitched into the inferred connected component
    std::deque<std::tuple<hd_t /*m1*/, int /*m0 poly*/, int /*m0 he*/>> patch_poly_stitching_queue;

    // enough space for cut-mesh polygons
    std::vector<bool> m0_poly_already_enqueued(m0_polygons.size() - traced_sm_polygon_count, false); // i.e. in "patch_poly_stitching_queue"
    const int m0_poly_already_enqueued_size = (int)m0_poly_already_enqueued.size();

    // for each color  ("interior" / "exterior")
    for (std::map<char, std::vector<int>>::const_iterator color_to_patches_iter = color_to_patch.cbegin();
         color_to_patches_iter != color_to_patch.cend();
         ++color_to_patches_iter) {

        const char color_id = color_to_patches_iter->first;

        const cm_patch_location_t& location = SAFE_ACCESS(patch_color_label_to_location, color_id);

        if ((location == cm_patch_location_t::INSIDE && !(input.keep_fragments_sealed_inside || input.keep_fragments_sealed_inside_exhaustive)) || //
            (location == cm_patch_location_t::OUTSIDE && !(input.keep_fragments_sealed_outside || input.keep_fragments_sealed_outside_exhaustive))) {
            continue; // skip stitching of exterior/ interior patches as user desires.
        }

        MCUT_ASSERT(color_to_m1.count(color_id) == 1);
        // get the reference to the copy of "m1" to which patches of the current color will be stitched
        hmesh_t& m1_colored = SAFE_ACCESS(color_to_m1, color_id);

        m1_colored.reserve_for_additional_elements(cs_vtx_count);

        // create entry
        color_to_m0_to_m1_he_instances.insert(std::make_pair(color_id, std::unordered_map<hd_t, std::map<int, hd_t>>()));
        // ref to entry
        std::unordered_map<hd_t, std::map<int, hd_t>>& m0_to_m1_he_instances = SAFE_ACCESS(color_to_m0_to_m1_he_instances, color_id);
        m0_to_m1_he_instances.reserve(m0.number_of_halfedges());
        // copy all of the "m1_polygons" that were created before we got to the stitching stage
        // Note: Before stitching has began, "m1_polygons" contains only source-mesh polygons,
        // which have been partition to allow separation of unsealed connected components
        std::pair<std::map<char, std::vector<traced_polygon_t>>::iterator, bool> color_to_m1_polygons_insertion = color_to_m1_polygons.insert(std::make_pair(color_id, m1_polygons)); // copy!

        MCUT_ASSERT(color_to_m1_polygons_insertion.second == true);

        // ref to "m1_polygons" i.e. the source-mesh polygons with partitioning
        std::vector<traced_polygon_t>& m1_polygons_colored = color_to_m1_polygons_insertion.first->second;
        m1_polygons_colored.reserve(m1_polygons_colored.size() + cs_face_count);

        // reference to the list connected components (see declaration for details)
        std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>& separated_stitching_CCs = color_to_separated_connected_ccsponents[color_id]; // insert

        std::unordered_map<int, int>& m0_to_m1_face_colored = SAFE_ACCESS(color_to_m0_to_m1_face, color_id); // note: containing mappings only for traced source mesh polygons initially!
        m0_to_m1_face_colored.reserve(m0_polygons.size());
        std::unordered_map<int, int>& m1_to_m0_face_colored = SAFE_ACCESS(color_to_m1_to_m0_face, color_id);
        m1_to_m0_face_colored.reserve(m0_polygons.size());
        MCUT_ASSERT(!m0_to_m1_face_colored.empty());
        MCUT_ASSERT(!m1_to_m0_face_colored.empty());

        std::vector<vd_t>& m1_to_m0_sm_ovtx_colored = SAFE_ACCESS(color_to_m1_to_m0_sm_ovtx, color_id);
        // MCUT_ASSERT(!m0_to_m1_sm_ovtx_colored.empty());
        MCUT_ASSERT(!m1_to_m0_sm_ovtx_colored.empty());
        m1_to_m0_sm_ovtx_colored.reserve(sm_vtx_cnt);

        // An original in "m0" that is used to trace a cut-mesh polygon will have
        // two "m1" versions - one for the ccw/normal patch and the other for the
        // cw/reversed patch.
        //
        // This map works like "color_to_m0_to_m1_sm_ovtx" but the difference is that each
        // "m0" vertex has two "m1" copies because we generate ccw & cw patches.

        MCUT_ASSERT(colour_to_m1_to_m0_cm_ovtx.count(color_id) == 0);
        std::unordered_map<vd_t, // "m1" cut-mesh vtx instance
            vd_t // "m0" cut-mesh ovtx instance
            >& m1_to_m0_cm_ovtx_colored
            = colour_to_m1_to_m0_cm_ovtx[color_id];

        // keeps track of the total number of cut-mesh polygons for the current color tag (interior/ext)
        int stitched_poly_counter = 0;

        // for each patch with current color
        for (std::vector<int>::const_iterator patch_iter = color_to_patches_iter->second.cbegin();
             patch_iter != color_to_patches_iter->second.cend();
             ++patch_iter) {

            // get patch index
            const int cur_patch_idx = *patch_iter;

            // is it a ccw/normal patch i.e. not the cw/reversed version
            // NOTE: ccw/normal patches are created/traced before reversed counterparts (hence the modulo trick)
            const bool is_ccw_patch = ((cur_patch_idx % total_ccw_patch_count) == cur_patch_idx);

            MCUT_ASSERT(patches.find(cur_patch_idx) != patches.cend());
            ///////////////////////////////////////////////////////////////////////////
            // stitch patch into a connected component stored in "m1_colored"
            ///////////////////////////////////////////////////////////////////////////

            //
            // We are basically going to search for the connected component (in "m1_colored") to which
            // the current patch will be stitched/glued.
            //
            // PERSONAL NOTE REGARDING `NORMAL` PATCHES:
            // Interior patches are stitched to the connected components which "naturally"
            // match their winding order (i.e. they are stitched to the connected component
            // "below" the patch).
            // Exterior patches are stitched to connected components which DO NOT share
            // the "natural" winding order (i.e. they are stitched to the connected component
            // "above" the patch).
            //

            MCUT_ASSERT(patch_to_seed_poly_idx.find(cur_patch_idx) != patch_to_seed_poly_idx.cend());

            // get the seed polygon from which to begin the stitching
            // this polygon will be on the patch boundary/border
            const int m0_patch_seed_poly_idx = SAFE_ACCESS(patch_to_seed_poly_idx, cur_patch_idx);

            // patch must contain the polygon
            MCUT_ASSERT(std::find(SAFE_ACCESS(patches, cur_patch_idx).cbegin(), SAFE_ACCESS(patches, cur_patch_idx).cend(), m0_patch_seed_poly_idx) != SAFE_ACCESS(patches, cur_patch_idx).cend());
            // the seed polygon must be from the ones that were traced in "m0" (see graph discovery stage above)
            MCUT_ASSERT(m0_patch_seed_poly_idx < (int)m0_polygons.size());

            // get the seed polygon of the patch
            const traced_polygon_t& m0_patch_seed_poly = SAFE_ACCESS(m0_polygons, m0_patch_seed_poly_idx);

            // patch must have a seed halfedge (the one used to traced the seed polygon)
            MCUT_ASSERT(patch_to_seed_interior_ihalfedge_idx.find(cur_patch_idx) != patch_to_seed_interior_ihalfedge_idx.cend());

            // get the index of the seed interior intersection halfedge of the patch
            // this is a halfedge defining the border of the patch and is used to trace
            // the seed polygon
            const int m0_patch_seed_poly_he_idx = SAFE_ACCESS(patch_to_seed_interior_ihalfedge_idx, cur_patch_idx);

            // must be within the range of the number of halfedge defining the seed polygon
            MCUT_ASSERT(m0_patch_seed_poly_he_idx < (int)m0_patch_seed_poly.size());

            // get the seed halfedge descriptor
            const hd_t& m0_patch_seed_poly_he = SAFE_ACCESS(m0_patch_seed_poly, m0_patch_seed_poly_he_idx);

            //
            // Here, we now deduce the connected component to which the current patch will be stitched.
            // To do this we can use the opposite halfedge of the seed halfedge. This opposite halfedge
            // is used to trace a source-mesh polygon next to the cut-path.
            //

            // get opposite halfedge of the seed halfedge of the current patch
            const hd_t m0_patch_seed_poly_he_opp = m0.opposite(m0_patch_seed_poly_he);

            // an "m1" version of this opposite halfedge must exist from the halfedge
            // partitioning problem we solved when duplicating intersection points to
            // partition the source-mesh
            MCUT_ASSERT(m0_to_m1_ihe.find(m0_patch_seed_poly_he_opp) != m0_to_m1_ihe.cend());

            // get the "m1" version of the opposite-halfedge of the seed-halfedge.
            // Note that this halfedge has already been used to trace a source-mesh polygon
            // in "m1"....
            const hd_t m1_seed_interior_ihe_opp = SAFE_ACCESS(m0_to_m1_ihe, m0_patch_seed_poly_he_opp);
            // .... thus, we have to use its opposite, which will be the "m1" version of the
            // seed halfedge of the current patch.
            // PERSONAL NOTE: this probably requires a visual example to properly understand
            const hd_t m1_seed_interior_ihe_opp_opp = m1_colored.opposite(m1_seed_interior_ihe_opp); // i.e. m1 instance of m0_patch_seed_poly_he_opp

            patch_poly_stitching_queue.clear();

            // reset
            for (int i = 0; i < m0_poly_already_enqueued_size; ++i) {
                m0_poly_already_enqueued[i] = false;
            }

            // thus, the first element is the seed polygon and the seed halfedge
            patch_poly_stitching_queue.push_back(
                std::make_tuple(
                    m1_seed_interior_ihe_opp_opp,
                    m0_patch_seed_poly_idx,
                    m0_patch_seed_poly_he_idx));

            //
            // In the following loop, we will stitch patch polygons iteratively as we
            // discover adjacent ones starting from the seed polygon. In each interation,
            // we process halfedges of the current polygon so that they reference the
            // correct vertex descriptors (src and tgt) in order to fill holes.
            //
            do {

                // the first processed/stitched of halfedge the current polygon (our starting point)
                hd_t m1_cur_patch_cur_poly_1st_he = hmesh_t::null_halfedge();
                int m0_cur_patch_cur_poly_idx = -1; // index into m0_polygons
                int m0_cur_patch_cur_poly_1st_he_idx = -1; // index into m0_polygon

                // pop element from queue (the next polygon to stitch)
                std::tie(
                    m1_cur_patch_cur_poly_1st_he,
                    m0_cur_patch_cur_poly_idx,
                    m0_cur_patch_cur_poly_1st_he_idx)
                    = patch_poly_stitching_queue.front();

                m0_poly_already_enqueued[(std::size_t)m0_cur_patch_cur_poly_idx - traced_sm_polygon_count] = true;

                // must be within the range of the traced polygons (include the reversed ones)
                MCUT_ASSERT(m0_cur_patch_cur_poly_idx < (int)m0_polygons.size());

                // get the current polygon of the patch
                const traced_polygon_t& m0_cur_patch_cur_poly = SAFE_ACCESS(m0_polygons, m0_cur_patch_cur_poly_idx);

                // the index of the starting halfedge must be within range of the polygon
                MCUT_ASSERT(m0_cur_patch_cur_poly_1st_he_idx < (int)m0_cur_patch_cur_poly.size());

                // get the descriptor of the starting halfedge
                const hd_t& m0_cur_patch_cur_poly_1st_he = SAFE_ACCESS(m0_cur_patch_cur_poly, m0_cur_patch_cur_poly_1st_he_idx);

                // the processed/stitched version of the current polygon
                m1_polygons_colored.emplace_back(traced_polygon_t());
                traced_polygon_t& m1_poly = m1_polygons_colored.back(); // stitched/"m1" version of polygon
                m1_poly.reserve(m0_cur_patch_cur_poly.size());
                m1_poly.push_back(m1_cur_patch_cur_poly_1st_he);

                // save mapping
                MCUT_ASSERT(m0_to_m1_face_colored.count(m0_cur_patch_cur_poly_idx) == 0);
                const int m1_cur_patch_cur_poly_idx = (int)(m1_polygons_colored.size() - 1);
                m0_to_m1_face_colored[m0_cur_patch_cur_poly_idx] = m1_cur_patch_cur_poly_idx;
                MCUT_ASSERT(m1_to_m0_face_colored.count(m1_cur_patch_cur_poly_idx) == 0);
                m1_to_m0_face_colored[m1_cur_patch_cur_poly_idx] = m0_cur_patch_cur_poly_idx;

                // the number of halfedges in the current polygon that have been processed
                // Note: we start from "1" because the initial halfedge (m0_cur_patch_cur_poly_1st_he) has already been processed.
                // That is, we already have an "m1" version of it thanks to the halfedge transformation step (intersection point
                // dupication step) which occurs along the cutpath.
                int transformed_he_counter = 1; //

                //
                // In the following loop, we will process polygon-halfedges iteratively as we
                // advance onto the "next" ones in the sequence starting from the initial. In each interation,
                // we create an "m1" version of the of the current "m0" halfedge so that it references the
                // correct vertex descriptors (src and tgt). The next iteration moves onto the
                // next halfedge, and so on...
                //

                do { // for each remaining halfedge of current polygon being stitched

                    // if (transformed_he_counter == 1) { // are we processing the second halfedge?
                    //  log
                    //  TODO: proper printing functions

                    //  << m1_colored.source(m1_cur_patch_cur_poly_1st_he) << " " << m1_colored.target(m1_cur_patch_cur_poly_1st_he) << ">" << std::endl;);
                    //}

                    // index of current halfedge index to be processed
                    const int m0_cur_patch_cur_poly_cur_he_idx = wrap_integer(m0_cur_patch_cur_poly_1st_he_idx + transformed_he_counter, 0, (int)m0_cur_patch_cur_poly.size() - 1);

                    // must be in range of polygon size
                    MCUT_ASSERT(m0_cur_patch_cur_poly_cur_he_idx < (int)m0_cur_patch_cur_poly.size());

                    // descriptor of current halfedge
                    const hd_t m0_cur_patch_cur_poly_cur_he = SAFE_ACCESS(m0_cur_patch_cur_poly, m0_cur_patch_cur_poly_cur_he_idx); // current untransformed
                    // opposite of current halfedge
                    const hd_t m0_cur_patch_cur_poly_cur_he_opp = m0.opposite(m0_cur_patch_cur_poly_cur_he);
                    // target of the current halfedge
                    vd_t m0_cur_patch_cur_poly_cur_he_tgt = m0.target(m0_cur_patch_cur_poly_cur_he);
                    vd_t m0_cur_patch_cur_poly_cur_he_src = m0.source(m0_cur_patch_cur_poly_cur_he);
                    const bool src_is_ivertex = m0_is_intersection_point(m0_cur_patch_cur_poly_cur_he_src, ps_vtx_cnt);
                    const bool tgt_is_ivertex = m0_is_intersection_point(m0_cur_patch_cur_poly_cur_he_tgt, ps_vtx_cnt);

                    // is the current halfedge the last to be processed in the current polygon?
                    const bool cur_is_last_to_be_transformed = ((transformed_he_counter + 1) == (int)m0_cur_patch_cur_poly.size()); // i.e. current he is last one to be transform
                    // enumerator of previously processed halfedge
                    const int m1_cur_patch_cur_poly_prev_he_idx = transformed_he_counter - 1; // note: transformed_he_counter is init to 1

                    // must be in current polygon's range
                    MCUT_ASSERT(m1_cur_patch_cur_poly_prev_he_idx >= 0 && m1_cur_patch_cur_poly_prev_he_idx < (int)m1_poly.size());

                    // get descriptor of the processed copy of the preceeding halfedge in the current polygon
                    const hd_t m1_cur_patch_cur_poly_prev_he = SAFE_ACCESS(m1_poly, m1_cur_patch_cur_poly_prev_he_idx); // previously transformed
                    // get target of transformed previous
                    const vd_t m1_cur_patch_cur_poly_prev_he_tgt = m1_colored.target(m1_cur_patch_cur_poly_prev_he); // transformed target of previous

                    ///////////////////////////////////////////////////////////////////////////
                    // create "m1" version of current halfedge
                    ///////////////////////////////////////////////////////////////////////////

                    // Note that the source of the processed/"m1" version of the current halfedge is the same as
                    // the target of the processed/"m1" version of the previous halfedge in the current polygon
                    vd_t m1_cs_cur_patch_polygon_he_src = m1_cur_patch_cur_poly_prev_he_tgt; // known from previous halfedge
                    // This initialization assumes the target of the processed/"m1" version of the current halfedge
                    // is the same descriptor as the unprocessed/"m0" version (this is generally true
                    // when processing non-boundary/border halfedges and the current patch is a normal patch).
                    vd_t m1_cs_cur_patch_polygon_he_tgt = m0_cur_patch_cur_poly_cur_he_tgt;

                    // flag whether to insert new edge into "m1_colored"
                    // bool create_new_edge = false;

                    hd_t m1_cur_patch_cur_poly_cur_he = hmesh_t::null_halfedge();

                    // is the current halfedge the last one to be process in the current polygon?
                    if (cur_is_last_to_be_transformed) {

                        // we can infer the updated version of the target vertex from the halfedge
                        // which is already updated. Update tgt will be the source of the first
                        // updated halfedge of the current polygon.
                        m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_poly[0]);

                        if (src_is_ivertex && tgt_is_ivertex) { // class 3 : // x-->x

                            //
                            // we now want to check if the current halfedge is interior or exterior
                            //

                            // MCUT_ASSERT(m0_ivtx_to_ps_edge.find(m0.source(m0_cur_patch_cur_poly_cur_he)) != m0_ivtx_to_ps_edge.cend());

                            // get the ps-halfedge in the intersection-registry entry of src
                            // const hd_t src_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(m0_cur_patch_cur_poly_cur_he));

                            // MCUT_ASSERT(m0_ivtx_to_ps_edge.find(m0.target(m0_cur_patch_cur_poly_cur_he)) != m0_ivtx_to_ps_edge.cend());

                            // get the ps-halfedge in the intersection-registry entry of src
                            // const hd_t tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.target(m0_cur_patch_cur_poly_cur_he));
                            // get the ps-edges corresponding to the ps-halfedges
                            vd_t src_vertex = m0_cur_patch_cur_poly_cur_he_src; // m0.source(m0_cur_patch_cur_poly_cur_he); // TODO: no need to query m0 [again] (see above)
                            MCUT_ASSERT((size_t)src_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(src_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                            const std::pair<ed_t, fd_t>& src_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)src_vertex - ps_vtx_cnt);
                            const ed_t src_ps_edge = src_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(m0_cur_patch_cur_poly_cur_he)); // ps.edge(src_coincident_ps_halfedge);

                            vd_t tgt_vertex = m0_cur_patch_cur_poly_cur_he_tgt; // m0.target(m0_cur_patch_cur_poly_cur_he);
                            MCUT_ASSERT((size_t)tgt_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(tgt_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                            const std::pair<ed_t, fd_t>& tgt_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)tgt_vertex - ps_vtx_cnt);
                            const ed_t tgt_ps_edge = tgt_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.target(m0_cur_patch_cur_poly_cur_he)); // ps.edge(tgt_ps_h);

                            // is it an interior halfedge
                            bool is_valid_ambiguious_interior_edge = (src_ps_edge != tgt_ps_edge);

                            if (is_valid_ambiguious_interior_edge) { // check is interior ihalfedge (due ambiguity with exterior-interior halfedges x-->x)

                                MCUT_ASSERT(m0_to_m1_ihe.find(m0_cur_patch_cur_poly_cur_he_opp) != m0_to_m1_ihe.cend());

                                const hd_t m1_cur_patch_cur_poly_cur_he_opp = SAFE_ACCESS(m0_to_m1_ihe, m0_cur_patch_cur_poly_cur_he_opp);
                                const hd_t m1_cur_patch_cur_poly_cur_he_opp_opp = m1_colored.opposite(m1_cur_patch_cur_poly_cur_he_opp);

                                // halfedge already exists. it was created during source-mesh partitioning stage earlier
                                m1_cur_patch_cur_poly_cur_he = m1_cur_patch_cur_poly_cur_he_opp_opp;

                                MCUT_ASSERT(m1_colored.target(m1_cur_patch_cur_poly_cur_he) == m1_cs_cur_patch_polygon_he_tgt);
                            }
                        }
                    } // if (cur_is_last_to_be_transformed) {
                    else if (!src_is_ivertex && tgt_is_ivertex) { // class 1 : o-->x : this type of halfedge can only be "coming in" i.e. pointing torward source mesh
                        // o-->x

                        /*
                        Steps:

                        transformed_src = transformed_prev_tgt // always available since cut-mesh polygon updating always starts from a halfedge whose opposite is already updated
                        transformed_tgt = untransformed_tgt // assume descriptor will not be updated
                        create_new_edge = FALSE // to insert a new edge into halfedge data structure or not

                        IF "opp" has been transformed // note: if opp is transformed, then polygon coincident to that opp halfedge has been fully updated too since all halfedges of a cut-mesh polygon are transformed before moving onto others.
                            transformed_tgt = source of transformed "opp"
                        ELSE
                            transformed_tgt = source of transformed "next" // note: "next" will always be an interior intersection-halfedge since o-->x ihalfedges are always "incoming" i.e. torward the src-mesh
                            create_new_edge = TRUE // because opposite does not exist (in "m1")

                        IF create_new_edge
                            create new edge and use halfedge defined by transformed_src and transformed_tgt
                        ELSE
                            use halfedge defined by computed transformed_src and transformed_tgt
                    */

                        // check if opposite halfedge of current is updated. (NOTE: searching only through
                        // the polygons of the current patch)

                        // the updated opposite halfedge in the current patch
                        hd_t m1_cs_cur_patch_polygon_he_opp = hmesh_t::null_halfedge();
                        // query instances of the "m1" version of the opposite halfedge
                        std::unordered_map<hd_t /*m0*/, std::map<int /*patch idx*/, hd_t /*m1*/>>::const_iterator m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_cur_patch_cur_poly_cur_he_opp);

                        // do we have at least one updated copy of the opposite, irrespective of which patch it
                        // belongs to.
                        if (m0_to_m1_he_instances_find_iter != m0_to_m1_he_instances.cend()) {
                            // now check if there is an updated instance corresponding to the current patch
                            std::map<int /*initial patch polygon*/, hd_t /*m1*/>::const_iterator m1_he_instances_find_iter = m0_to_m1_he_instances_find_iter->second.find(cur_patch_idx);
                            if (m1_he_instances_find_iter != m0_to_m1_he_instances_find_iter->second.cend()) {
                                // we have found the already-updated instance of the opposite halfedge
                                m1_cs_cur_patch_polygon_he_opp = m1_he_instances_find_iter->second;
                            }
                        }

                        // check if opposite halfedge is transformed
                        const bool opp_is_transformed = m1_cs_cur_patch_polygon_he_opp != hmesh_t::null_halfedge();

                        if (opp_is_transformed) {
                            // infer tgt from opposite
                            m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_cs_cur_patch_polygon_he_opp);
                        } else {

                            //
                            // the opposite halfedge has not been transformed.
                            // We will deduce the target from the updated "next" halfedge, and
                            // we have to create a new edge
                            //

                            // look up the updated "next" by looking forward and finding the coincident source-mesh polygon
                            // and then getting the updated instance of "next".
                            const int m0_next_cs_polygon_he_index = wrap_integer(m0_cur_patch_cur_poly_cur_he_idx + 1, 0, (int)m0_cur_patch_cur_poly.size() - 1);

                            MCUT_ASSERT(m0_next_cs_polygon_he_index < (int)m0_cur_patch_cur_poly.size());

                            const hd_t m0_cs_next_patch_polygon_he = SAFE_ACCESS(m0_cur_patch_cur_poly, m0_next_cs_polygon_he_index); // next untransformed
                            const vd_t m0_cs_next_patch_polygon_he_src = m0.source(m0_cs_next_patch_polygon_he);
                            const vd_t m0_cs_next_patch_polygon_he_tgt = m0.target(m0_cs_next_patch_polygon_he);

                            MCUT_ASSERT(m0_is_intersection_point(m0_cs_next_patch_polygon_he_src, ps_vtx_cnt) && m0_is_intersection_point(m0_cs_next_patch_polygon_he_tgt, ps_vtx_cnt)); // .. because the current halfedge is "incoming"

                            // get the "m0" polygons which are traced with the "next" halfedge
                            // const std::vector<int>& m0_poly_he_coincident_polys = SAFE_ACCESS(m0_h_to_ply, m0_cs_next_patch_polygon_he);
                            // get reference to src-mesn polygon which is traced with "next" halfedge
                            // const std::vector<int>::const_iterator find_iter = std::find_if(
                            //    m0_poly_he_coincident_polys.cbegin(),
                            //    m0_poly_he_coincident_polys.cend(),
                            //    [&](const int& e) {
                            //        return (e < traced_sm_polygon_count); // match with src-mesn polygon
                            //    });

                            // "next" is always incident to a source-mesh polygon
                            MCUT_ASSERT(std::find_if(
                                            SAFE_ACCESS(m0_h_to_ply, m0_cs_next_patch_polygon_he).cbegin(),
                                            SAFE_ACCESS(m0_h_to_ply, m0_cs_next_patch_polygon_he).cend(),
                                            [&](const int& e) {
                                                return (e < traced_sm_polygon_count); // match with src-mesn polygon
                                            })
                                != SAFE_ACCESS(m0_h_to_ply, m0_cs_next_patch_polygon_he).cend());

                            //
                            // At this point, we have found the adjacent connected component which is
                            // the one using m0_cs_next_patch_polygon_he. Therefore, we can directly
                            // determine the connected component by looking up the updated instance
                            // of m0_cs_next_patch_polygon_he_opp since m0_cs_next_patch_polygon_he_opp
                            // is guarranteed to have been updated because it is an interior intersection
                            // halfedge (i.e. its on the cut path).
                            //
                            // REMEMBER: exterior patches are stitched to the "upper" source-mesh fragment
                            const hd_t m0_cs_next_patch_polygon_he_opp = m0.opposite(m0_cs_next_patch_polygon_he);
                            const hd_t m1_cs_next_patch_polygon_he_opp = SAFE_ACCESS(m0_to_m1_ihe, m0_cs_next_patch_polygon_he_opp);
                            const hd_t m1_cs_next_patch_polygon_he_opp_opp = m1_colored.opposite(m1_cs_next_patch_polygon_he_opp);

                            m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_cs_next_patch_polygon_he_opp_opp);

                            // create_new_edge = true; // because opposite has not yet been created
                        }
                    } else if (src_is_ivertex && tgt_is_ivertex) { // class 3 : // x-->x

                        // the current halfedge will either be interior or exterior.

                        // MCUT_ASSERT(m0_ivtx_to_ps_edge.find(m0.source(m0_cur_patch_cur_poly_cur_he)) != m0_ivtx_to_ps_edge.cend());

                        // const hd_t src_coincident_ps_halfedge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(m0_cur_patch_cur_poly_cur_he));

                        // MCUT_ASSERT(m0_ivtx_to_ps_edge.find(m0.target(m0_cur_patch_cur_poly_cur_he)) != m0_ivtx_to_ps_edge.cend());

                        // const hd_t tgt_ps_h = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.target(m0_cur_patch_cur_poly_cur_he));
                        const vd_t src_vertex = m0_cur_patch_cur_poly_cur_he_src; // m0.source(m0_cur_patch_cur_poly_cur_he); // TODO: no need to query m0 [again] (see above)
                        MCUT_ASSERT((size_t)src_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(src_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t>& src_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)src_vertex - ps_vtx_cnt);
                        const ed_t src_ps_edge = src_vertex_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(m0_cur_patch_cur_poly_cur_he)); // ps.edge(src_coincident_ps_halfedge);

                        const vd_t tgt_vertex = m0_cur_patch_cur_poly_cur_he_tgt; // m0.target(m0_cur_patch_cur_poly_cur_he);
                        MCUT_ASSERT((size_t)tgt_vertex - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(tgt_vertex) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t>& tgt_vertex_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)tgt_vertex - ps_vtx_cnt);
                        const ed_t tgt_ps_edge = tgt_vertex_ipair.first;

                        // const ed_t src_ps_edge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.source(m0_cur_patch_cur_poly_cur_he)); //ps.edge(src_coincident_ps_halfedge);
                        // const ed_t tgt_ps_edge = SAFE_ACCESS(m0_ivtx_to_ps_edge, m0.target(m0_cur_patch_cur_poly_cur_he)); // ps.edge(tgt_ps_h);
                        bool is_valid_ambiguious_interior_edge = (src_ps_edge != tgt_ps_edge);

                        // check if current halfedge is interior
                        if (is_valid_ambiguious_interior_edge) {

                            MCUT_ASSERT(m0_to_m1_ihe.find(m0_cur_patch_cur_poly_cur_he_opp) != m0_to_m1_ihe.cend());
                            const hd_t m1_cur_patch_cur_poly_cur_he_opp = SAFE_ACCESS(m0_to_m1_ihe, m0_cur_patch_cur_poly_cur_he_opp);
                            const hd_t m1_cur_patch_cur_poly_cur_he_opp_opp = m1_colored.opposite(m1_cur_patch_cur_poly_cur_he_opp);

                            // halfedge already exists. it was created during src-mesh partitioning
                            m1_cur_patch_cur_poly_cur_he = m1_cur_patch_cur_poly_cur_he_opp_opp;
                            m1_cs_cur_patch_polygon_he_tgt = m1_colored.target(m1_cur_patch_cur_poly_cur_he_opp_opp);
                        } else { // its an exterior x-->x halfedge

                            // look up the transformed "next" by looking finding the
                            // coincident source-mesh polygon and then getting the transformed instance of "next".
                            const int m0_next_cs_polygon_he_index = wrap_integer(m0_cur_patch_cur_poly_cur_he_idx + 1, 0, (int)m0_cur_patch_cur_poly.size() - 1);

                            MCUT_ASSERT(m0_next_cs_polygon_he_index < (int)m0_cur_patch_cur_poly.size());

                            const hd_t m0_cs_next_patch_polygon_he = m0_cur_patch_cur_poly[m0_next_cs_polygon_he_index]; // next untransformed
                            const vd_t m0_cs_next_patch_polygon_he_src = m0.source(m0_cs_next_patch_polygon_he);
                            const vd_t m0_cs_next_patch_polygon_he_tgt = m0.target(m0_cs_next_patch_polygon_he);

                            MCUT_ASSERT(m0_is_intersection_point(m0_cs_next_patch_polygon_he_src, ps_vtx_cnt) && m0_is_intersection_point(m0_cs_next_patch_polygon_he_tgt, ps_vtx_cnt));
                            MCUT_ASSERT(SAFE_ACCESS(m0_h_to_ply, m0_cs_next_patch_polygon_he).size() > 0 /*m0_h_to_ply.find(m0_cs_next_patch_polygon_he) != m0_h_to_ply.cend()*/);

#ifndef NDEBUG
                            const std::vector<int>& m0_poly_he_coincident_polys = m0_h_to_ply[m0_cs_next_patch_polygon_he];
                            const std::vector<int>::const_iterator find_iter = std::find_if( // points to src-mesh polygon
                                m0_poly_he_coincident_polys.cbegin(),
                                m0_poly_he_coincident_polys.cend(),
                                [&](const int& e) {
                                    return (e < traced_sm_polygon_count); // match with source-mesh polygon
                                });

                            // "next" is always incident to an source-mesh polygon
                            MCUT_ASSERT(find_iter != m0_poly_he_coincident_polys.cend());
#endif
                            const hd_t m0_cs_next_patch_polygon_he_opp = m0.opposite(m0_cs_next_patch_polygon_he);

                            // Note: this is always true, even in the case of scoop cuts. This is because
                            // halfedges along the cut-path are updated before stitching (during source-mesh partitioning)
                            // so we can infer the tgt easily
                            MCUT_ASSERT(SAFE_ACCESS(m0_h_to_ply, m0_cs_next_patch_polygon_he_opp).size() > 0 /*m0_h_to_ply.find(m0_cs_next_patch_polygon_he_opp) != m0_h_to_ply.cend()*/);

                            const hd_t m1_cs_next_patch_polygon_he_opp = SAFE_ACCESS(m0_to_m1_ihe, m0_cs_next_patch_polygon_he_opp);
                            const hd_t m1_cs_next_patch_polygon_he_opp_opp = m1_colored.opposite(m1_cs_next_patch_polygon_he_opp);

                            m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_cs_next_patch_polygon_he_opp_opp);
                        }
                    } else { // class 0 or 2 i.e. o-->o or x-->o

                        /*
                        In the following steps, our ability to deduce the correct target vertex instance
                        by simply checking whether "opp" or "next" is updated before
                        duplication is guarranteed to work. This is because we update polygons
                        of a patch using BFS (following adjacency) which guarrantees that when
                        the condition to create a duplicate vertex is reached, there will have
                        been no other halfedge referencing the same vertex that had reached the
                        same condition.

                        transformed_src = transformed_prev_tgt // always available because cut-mesh polygon update always starts from a halfedge whose opposite is already updated
                        transformed_tgt = untransformed_tgt
                        create_new_edge = FALSE

                        IF opposite patch is transformed
                            1. IF opposite halfedge is transformed
                            2.      infer from opposite halfedge
                            3. ELSE IF next halfedge is transformed
                            4.      infer from next
                            2. ELSE
                                IF an updated halfedge pointing to tgt already exists (i.e. using "halfedges around vertex")
                                    infer from that halfedge
                                ELSE
                                    create duplicate of untransformed_tgt
                                    transformed_tgt = duplicate of untransformed_tgt
                                    create_new_edge = TRUE // because "opposite" AND "next" halfedge are not updated, so we have create a new connection between vertices
                        ELSE
                                // Do nothing (keep transformed_tgt as it is) because there
                                // is no adjacent halfedge which is updated, and the current
                                // patch gets precedence to use the first/original vertex instances
                    */
                        if (cur_is_last_to_be_transformed) {
                            // initial polygon halfedge which was transformed
                            m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_poly.front());
                        } else {

                            // check opposite patch of current is transformed
                            const bool opposite_patch_is_transformed = !is_ccw_patch; // ... since ccw patches are always transformed before their cw counterparts

                            if (opposite_patch_is_transformed) // if true, the current patch is the cw one
                            {
                                // check if opposite halfedge of current is transformed. (NOTE: searching
                                // only through the polygons of the current patch)

                                hd_t m1_cs_cur_patch_polygon_he_opp = hmesh_t::null_halfedge(); // transformed instance of opposite
                                std::unordered_map<hd_t /*m0*/, std::map<int /*patch idx*/, hd_t /*m1*/>>::const_iterator m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_cur_patch_cur_poly_cur_he_opp);

                                if (m0_to_m1_he_instances_find_iter != m0_to_m1_he_instances.cend()) { // must transformed at least once since opposite patch is transformed

                                    std::map<int /*initial patch polygon*/, hd_t /*m1*/>::const_iterator m1_he_instances_find_iter = m0_to_m1_he_instances_find_iter->second.find(cur_patch_idx);

                                    if (m1_he_instances_find_iter != m0_to_m1_he_instances_find_iter->second.cend()) {
                                        m1_cs_cur_patch_polygon_he_opp = m1_he_instances_find_iter->second;
                                    }
                                }

                                const bool opp_is_transformed = m1_cs_cur_patch_polygon_he_opp != hmesh_t::null_halfedge();

                                if (opp_is_transformed) {
                                    m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_cs_cur_patch_polygon_he_opp);
                                } else {

                                    // check if next halfedge of current is transformed.
                                    const int m0_next_cs_polygon_he_index = wrap_integer(m0_cur_patch_cur_poly_cur_he_idx + 1, 0, (int)m0_cur_patch_cur_poly.size() - 1);
                                    const hd_t m0_cs_next_patch_polygon_he = m0_cur_patch_cur_poly[m0_next_cs_polygon_he_index]; // next untransformed
                                    m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_cs_next_patch_polygon_he);
                                    hd_t m1_cs_next_patch_polygon_he = hmesh_t::null_halfedge();

                                    if (m0_to_m1_he_instances_find_iter != m0_to_m1_he_instances.cend()) { // must transformed at least once since opposite patch is transformed

                                        std::map<int, hd_t>::const_iterator m1_he_instances_find_iter = m0_to_m1_he_instances_find_iter->second.find(cur_patch_idx);

                                        if (m1_he_instances_find_iter != m0_to_m1_he_instances_find_iter->second.cend()) {
                                            m1_cs_next_patch_polygon_he = m1_he_instances_find_iter->second;
                                        }
                                    }

                                    const bool next_is_transformed = m1_cs_next_patch_polygon_he != hmesh_t::null_halfedge();

                                    if (next_is_transformed) {
                                        m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_cs_next_patch_polygon_he);
                                    } else {

                                        //
                                        // find all transformed halfedges which connect to m0_cur_patch_cur_poly_cur_he_tgt in the current patch
                                        //

                                        bool found_transformed_neigh_he = false; // any updated halfedge whose m0 instance references m0_cur_patch_cur_poly_cur_he_tgt

                                        /*
                                      1. get "m0" halfedges around vertex
                                      2. for each halfedge around vertex, check if it has a transformed instance that belonging to the current patch
                                    */
                                        const std::vector<halfedge_descriptor_t>& m0_incoming_halfedges = m0.get_halfedges_around_vertex(m0_cur_patch_cur_poly_cur_he_tgt);

                                        for (std::vector<halfedge_descriptor_t>::const_iterator m0_incoming_halfedges_iter = m0_incoming_halfedges.cbegin();
                                             m0_incoming_halfedges_iter != m0_incoming_halfedges.cend();
                                             ++m0_incoming_halfedges_iter) {

                                            const halfedge_descriptor_t m0_incoming_halfedge = *m0_incoming_halfedges_iter;
                                            MCUT_ASSERT(m0_incoming_halfedge != hmesh_t::null_halfedge());

                                            // is it transformed?
                                            m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_incoming_halfedge);
                                            hd_t m1_incoming_halfedge = hmesh_t::null_halfedge();

                                            if (m0_to_m1_he_instances_find_iter != m0_to_m1_he_instances.cend()) {

                                                // get the transformed instance belonging to the current patch
                                                std::map<int, hd_t>::const_iterator m1_he_instances_find_iter = m0_to_m1_he_instances_find_iter->second.find(cur_patch_idx);

                                                if (m1_he_instances_find_iter != m0_to_m1_he_instances_find_iter->second.cend()) {
                                                    m1_incoming_halfedge = m1_he_instances_find_iter->second;

                                                    m1_cs_cur_patch_polygon_he_tgt = m1_colored.target(m1_incoming_halfedge);
                                                    found_transformed_neigh_he = true;
                                                }
                                            }

                                            if (!found_transformed_neigh_he) {
                                                // We enter this scope if: "m1_incoming_halfedge" does not exist

                                                // What we are going to try to do now is check if the opposite of "m0_incoming_halfedge" has been transformed (w.r.t the current patch),
                                                // and if so, we get it transformed instanced from which we deduce the correct value of "m1_cs_cur_patch_polygon_he_tgt"

                                                const halfedge_descriptor_t m0_incoming_halfedge_opp = m0.opposite(m0_incoming_halfedge);
                                                m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_incoming_halfedge);
                                                hd_t m1_incoming_halfedge_opp = hmesh_t::null_halfedge();

                                                if (m0_to_m1_he_instances_find_iter != m0_to_m1_he_instances.cend()) {

                                                    std::map<int, hd_t>::const_iterator m1_he_instances_find_iter = m0_to_m1_he_instances_find_iter->second.find(cur_patch_idx);

                                                    if (m1_he_instances_find_iter != m0_to_m1_he_instances_find_iter->second.cend()) {
                                                        m1_incoming_halfedge_opp = m1_he_instances_find_iter->second;
                                                        m1_cs_cur_patch_polygon_he_tgt = m1_colored.source(m1_incoming_halfedge_opp); // Note: using "m1.source" not "m1.target"
                                                        found_transformed_neigh_he = true;
                                                    }
                                                }
                                            }

                                            if (found_transformed_neigh_he) {
                                                break; // done
                                            }
                                        }

                                        if (!found_transformed_neigh_he) {
                                            //
                                            // none of the adjacent halfedges have been transformed, so we must duplicate m0_cur_patch_cur_poly_cur_he_tgt
                                            //

                                            // is "m1_cs_cur_patch_polygon_he_tgt" a vertex we can duplicate? (partial cut, interior sealing)
                                            // TODO: std::sort(sm_interior_cs_border_vertices) and then we can use binary search
                                            const bool is_sm_interior_cs_boundary_vertex = std::find(sm_interior_cs_border_vertices.cbegin(), sm_interior_cs_border_vertices.cend(), m0_cur_patch_cur_poly_cur_he_tgt) != sm_interior_cs_border_vertices.cend();

                                            if (!is_sm_interior_cs_boundary_vertex) {

                                                const vd_t m0_poly_he_tgt_dupl = m1_colored.add_vertex(m0.vertex(m0_cur_patch_cur_poly_cur_he_tgt));

                                                MCUT_ASSERT(m0_poly_he_tgt_dupl != hmesh_t::null_halfedge());

                                                m1_cs_cur_patch_polygon_he_tgt = m0_poly_he_tgt_dupl;
                                                // create_new_edge = true;
                                            }
                                        }
                                    } // if (next_is_transformed) {
                                } // if (opp_is_transformed) {
                            } // if (opposite_patch_is_transformed)
                        } // if (cur_is_last_to_be_transformed) {
                    } // class 0 or 2 i.e. o-->o or x-->o

                    // if we could not infer from any pre-existing halfedge
                    if (m1_cur_patch_cur_poly_cur_he == hmesh_t::null_halfedge()) {
                        // check if edge exists
                        // TODO: use mesh built "halfedge(...)" (may require minor update to function)
                        // ed_t e = get_computed_edge(/*m1_colored, */ m1_cs_cur_patch_polygon_he_src, m1_cs_cur_patch_polygon_he_tgt);
                        // hd_t h = m1_colored.halfedge(m1_cs_cur_patch_polygon_he_src, m1_cs_cur_patch_polygon_he_tgt);
                        ed_t e = m1_colored.edge(m1_cs_cur_patch_polygon_he_src, m1_cs_cur_patch_polygon_he_tgt, true);

                        if (e != hmesh_t::null_edge()) { // if edge already exists

                            hd_t h0 = m1_colored.halfedge(e, 0);

                            if (m1_colored.source(h0) == m1_cs_cur_patch_polygon_he_src) {
                                m1_cur_patch_cur_poly_cur_he = h0;
                            } else {
                                hd_t h1 = m1_colored.halfedge(e, 1);
                                m1_cur_patch_cur_poly_cur_he = h1;
                            }
                        } else {

                            m1_cur_patch_cur_poly_cur_he = m1_colored.add_edge(m1_cs_cur_patch_polygon_he_src, m1_cs_cur_patch_polygon_he_tgt);
                            // TODO:replace with map (for O(Log N) searches)
                            // m1_computed_edges.push_back(m1_colored.edge(m1_cur_patch_cur_poly_cur_he));
                            // std::map<vd_t, std::vector<std::pair<vd_t, ed_t>>>

                            // ed_t new_edge = m1_colored.edge(m1_cur_patch_cur_poly_cur_he);
                            // m1_computed_edges[m1_cs_cur_patch_polygon_he_src].push_back(std::make_pair(m1_cs_cur_patch_polygon_he_tgt, new_edge));
                            // m1_computed_edges[m1_cs_cur_patch_polygon_he_tgt].push_back(std::make_pair(m1_cs_cur_patch_polygon_he_src, new_edge));
                        }
                    } // if (m1_cur_patch_cur_poly_cur_he == hmesh_t::null_halfedge()) {

                    //  << m1_colored.source(m1_cur_patch_cur_poly_cur_he) << " " << m1_colored.target(m1_cur_patch_cur_poly_cur_he) << ">" << std::endl;);

                    // halfedge must have been found (created or inferred)
                    MCUT_ASSERT(m1_cur_patch_cur_poly_cur_he != hmesh_t::null_halfedge());
                    MCUT_ASSERT(m1_colored.target(m1_poly.back()) == m1_colored.source(m1_cur_patch_cur_poly_cur_he));

                    // add transformed halfedge to currently transformed polygon
                    m1_poly.push_back(m1_cur_patch_cur_poly_cur_he);

                    //
                    // map halfedge to transformed instance of current patch
                    //

                    // NOTE: m0_cur_patch_cur_poly_cur_he will not exist if current patch has CCW orientation
                    // since such a patch will always be transformed first before its CW counterpart.
                    std::unordered_map<hd_t, std::map<int, hd_t>>::iterator m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_cur_patch_cur_poly_cur_he);

                    if (m0_to_m1_he_instances_find_iter == m0_to_m1_he_instances.end()) { // not yet transformed at all (i.e. m0_cur_patch_cur_poly_cur_he belongs to CCW polygon )

                        std::pair<std::unordered_map<hd_t /*m0*/, std::map<int /*initial patch polygon*/, hd_t /*m1*/>>::iterator, bool> pair = m0_to_m1_he_instances.insert(std::make_pair(m0_cur_patch_cur_poly_cur_he, std::map<int, hd_t>()));

                        MCUT_ASSERT(pair.second == true);

                        m0_to_m1_he_instances_find_iter = pair.first;
                    }

                    // stores the an "m1" instance of the current halfedge, for each patch
                    std::map<int, hd_t /*m1*/>& patch_to_m1_he = m0_to_m1_he_instances_find_iter->second;
                    // const std::map<int, hd_t /*m1*/>::const_iterator patch_idx_to_m1_he = patch_to_m1_he.find(cur_patch_idx);

                    // In general, a halfedge may only be transformed once for each patch it is be associated
                    // with (i.e it will have two copies with one for each opposing patch). Note however that
                    // in the case that the current halfedge is a border halfedge (partial cut), its transformed
                    // copy is the same as its untransformed copy for each patch
                    MCUT_ASSERT(patch_to_m1_he.find(cur_patch_idx) == patch_to_m1_he.cend());

                    patch_to_m1_he.insert(std::make_pair(cur_patch_idx, m1_cur_patch_cur_poly_cur_he));
                    transformed_he_counter += 1; // next halfedge in m0_cur_patch_cur_poly

                } while (transformed_he_counter != (int)m0_cur_patch_cur_poly.size()); // while not all halfedges of the current polygon have been transformed.

                //
                // at this stage, all halfedges of the current polygon have been transformed
                //

                // ... remove the stitching-initialiation data of current polygon.
                patch_poly_stitching_queue.pop_front();

                ///////////////////////////////////////////////////////////////////////////
                // find untransformed neighbouring polygons and queue them
                ///////////////////////////////////////////////////////////////////////////

                //
                // We are basically adding all unstitched neighbours of the current polygon
                // (that we just sticthed) to the queue so they can be stitched as well. These
                // are polygons on the same patch as the current polygon and are adjacent to
                // it i.e. they share an edge.
                //

                MCUT_ASSERT(patches.find(cur_patch_idx) != patches.cend());

                // polygons of current patch
                // const std::vector<int>& patch_polys = SAFE_ACCESS(patches, cur_patch_idx);

                // stores the queued adjacent polygon to be stitched that have just been discovered
                // i.e. discovered while finding the next untransformed adjacent polygons of the
                // current one that we just transformed
                std::deque<std::tuple<hd_t /*m1*/, int /*m0 poly*/, int /*m0 he*/>> patch_poly_stitching_queue_tmp;

                // for each halfedge of the polygon we just stitched
                for (traced_polygon_t::const_iterator m0_poly_he_iter = m0_cur_patch_cur_poly.cbegin();
                     m0_poly_he_iter != m0_cur_patch_cur_poly.cend();
                     ++m0_poly_he_iter) {

                    const hd_t m0_cur_patch_cur_poly_cur_he = *m0_poly_he_iter;

                    // Skip certain neighbours. The adjacent polygon has been processed (assuming
                    // it exists) if the following conditions are true. Theses conditions are
                    // evaluated on "m0_cur_patch_cur_poly_cur_he"
                    //
                    // 1. is same as initial halfedge
                    //      implies that opposite is already transformed (before the current polygon,
                    //      we transformed the polygon which is traced by the opposite halfedge of
                    //      m0_cur_patch_cur_poly_1st_he)
                    // 2. is interior intersection-halfedge
                    //      opposite is already transformed since interior intersection-halfedges
                    //      are on cut-path (the opposite halfedge is incident to one of the
                    //      src-mesh connected components)
                    // 3. is a halfedge whose opposite has been transformed
                    //      because that implies that its polygon has been transformed (so no need
                    //      to add to queue).
                    // 4. it is a border halfedge
                    //      (i.e. the only face incident to the opposite halfedge is another which
                    //      belong to the opposite patch)
                    //

                    //
                    // case 1
                    //
                    if (m0_cur_patch_cur_poly_cur_he == m0_cur_patch_cur_poly_1st_he) {
                        continue; // 1
                    }

                    //
                    // case 2
                    //
                    const vd_t m0_cur_patch_cur_poly_cur_he_src = m0.source(m0_cur_patch_cur_poly_cur_he);
                    const vd_t m0_cur_patch_cur_poly_cur_he_tgt = m0.target(m0_cur_patch_cur_poly_cur_he);
                    bool is_ambiguious_interior_edge_case = m0_is_intersection_point(m0_cur_patch_cur_poly_cur_he_src, ps_vtx_cnt) && m0_is_intersection_point(m0_cur_patch_cur_poly_cur_he_tgt, ps_vtx_cnt);

                    if (is_ambiguious_interior_edge_case) {
                        MCUT_ASSERT((size_t)m0_cur_patch_cur_poly_cur_he_src - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_cur_patch_cur_poly_cur_he_src) != m0_ivtx_to_intersection_registry_entry.cend()*/);

                        const std::pair<ed_t, fd_t>& m0_cur_patch_cur_poly_cur_he_src_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)m0_cur_patch_cur_poly_cur_he_src - ps_vtx_cnt);
                        const ed_t src_ps_edge = m0_cur_patch_cur_poly_cur_he_src_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_cur_patch_cur_poly_cur_he_src); //ps.edge(src_coincident_ps_halfedge);

                        MCUT_ASSERT((size_t)m0_cur_patch_cur_poly_cur_he_tgt - ps_vtx_cnt < m0_ivtx_to_intersection_registry_entry.size() /*m0_ivtx_to_intersection_registry_entry.find(m0_cur_patch_cur_poly_cur_he_tgt) != m0_ivtx_to_intersection_registry_entry.cend()*/);
                        const std::pair<ed_t, fd_t>& m0_cur_patch_cur_poly_cur_he_tgt_ipair = SAFE_ACCESS(m0_ivtx_to_intersection_registry_entry, (std::size_t)m0_cur_patch_cur_poly_cur_he_tgt - ps_vtx_cnt);

                        const ed_t tgt_ps_edge = m0_cur_patch_cur_poly_cur_he_tgt_ipair.first; // SAFE_ACCESS(m0_ivtx_to_ps_edge, m0_cur_patch_cur_poly_cur_he_tgt); //ps.edge(tgt_ps_h);

                        bool is_valid_ambiguious_interior_edge = (src_ps_edge != tgt_ps_edge);

                        if (is_valid_ambiguious_interior_edge) {
                            continue; // 2
                        }
                    }

                    //
                    // case 3
                    //
                    const hd_t m0_cur_patch_cur_poly_cur_he_opp = m0.opposite(m0_cur_patch_cur_poly_cur_he);
                    std::unordered_map<hd_t, std::map<int, hd_t>>::const_iterator m0_to_m1_he_instances_find_iter = m0_to_m1_he_instances.find(m0_cur_patch_cur_poly_cur_he_opp); // value will not exist if current patch positive

                    if (m0_to_m1_he_instances_find_iter != m0_to_m1_he_instances.cend()) { // check exists (i.e. m0_cur_patch_cur_poly_cur_he_opp has be transform but we dont know for which patch it has been transformed (CCW or CW)

                        const std::map<int, hd_t>& patch_to_m1_he = m0_to_m1_he_instances_find_iter->second;
                        std::map<int, hd_t>::const_iterator patch_idx_to_m1_he = patch_to_m1_he.find(cur_patch_idx);

                        if (patch_idx_to_m1_he != patch_to_m1_he.cend()) { // check is stitched
                            MCUT_ASSERT(patch_idx_to_m1_he->second != hmesh_t::null_halfedge());
                            continue; // 3
                        }
                    }

                    //
                    // case 4
                    //

                    // must exist because m0_cur_patch_cur_poly_cur_he was just transformed
                    MCUT_ASSERT(m0_to_m1_he_instances.find(m0_cur_patch_cur_poly_cur_he) != m0_to_m1_he_instances.cend());

                    // find m1_cur_polygon_he which is the transformed instance of m0_cur_patch_cur_poly_cur_he
                    /*const*/ std::map<int, hd_t>& patch_to_m1_he = SAFE_ACCESS(m0_to_m1_he_instances, m0_cur_patch_cur_poly_cur_he);

                    MCUT_ASSERT(patch_to_m1_he.find(cur_patch_idx) != patch_to_m1_he.cend());

                    const hd_t m1_cur_polygon_he = SAFE_ACCESS(patch_to_m1_he, cur_patch_idx);
                    // transformed halfedge used by adjacent polygon
                    const hd_t m1_next_poly_seed_he = m1_colored.opposite(m1_cur_polygon_he);

                    // infer the index of the next stitched polygon which is traced with m0_cur_patch_cur_poly_cur_he_opp
                    MCUT_ASSERT(SAFE_ACCESS(m0_h_to_ply, m0_cur_patch_cur_poly_cur_he_opp).size() > 0 /*m0_h_to_ply.find(m0_cur_patch_cur_poly_cur_he_opp) != m0_h_to_ply.cend()*/);

                    //
                    // find the adjacent polygon in the current patch using the opposite of the
                    // current halfedge
                    //

                    // get the polygons traced with the opposite halfedge
                    const std::vector<int> m0_poly_he_opp_coincident_polys = SAFE_ACCESS(m0_h_to_ply, m0_cur_patch_cur_poly_cur_he_opp);
                    const std::vector<int>::const_iterator find_iter = std::find_if( // find the current polygon of current patch
                        m0_poly_he_opp_coincident_polys.cbegin(),
                        m0_poly_he_opp_coincident_polys.cend(),
                        [&](const int poly_idx) {
                            MCUT_ASSERT(m0_cm_poly_to_patch_idx.count(poly_idx) == 1);
                            // return SAFE_ACCESS(m0_cm_poly_to_patch_idx, poly_idx) == cur_patch_idx;

                            bool has_patch_winding_orientation = false;

                            // check if polygon has the same winding order as the current patch

                            if (is_ccw_patch) { // is the current patch a "normal" patch?
                                has_patch_winding_orientation = (poly_idx < traced_polygon_count);
                            } else {
                                has_patch_winding_orientation = (poly_idx >= traced_polygon_count);
                            }

                            return has_patch_winding_orientation && SAFE_ACCESS(m0_cm_poly_to_patch_idx, poly_idx) == cur_patch_idx; // std::find(patch_polys.cbegin(), patch_polys.cend(), poly_idx) != patch_polys.cend(); // NOTE: only one polygon in the current patch will match
                        });

                    // note: if the current halfedge is on the border of the cut-mesh, then its opposite
                    // halfedge can only trace one polygon, which is the opposite polygon to the
                    // current (i.e. on the opposing patch). Hence, if find_iter is null then it means "m0_cur_patch_cur_poly_cur_he"
                    // is on the border of the cut-mesh.
                    const bool opp_is_border_halfedge = (find_iter == m0_poly_he_opp_coincident_polys.cend()); // current patch is reversed-patch and

                    if (opp_is_border_halfedge) {
                        // 4 there is no neighbouring polygon which is coincident to
                        // "m0_cur_patch_cur_poly_cur_he_opp"
                        continue;
                    }

                    // the adjacent polygon
                    const int m0_next_poly_idx = *find_iter;

                    //
                    // TODO: the following conditions below could also be speeded up if we
                    // create a tmp vector/map which stores all of the adjacent polygons we have
                    // already queued. Searching over this vector could be that bit faster.
                    // We could do the right here now that "m0_next_poly_idx" is known.
                    //

                    // deduce the index of the next polygon's seed m0 halfedge
                    // -------------------------------------------------------

                    MCUT_ASSERT(m0_next_poly_idx < (int)m0_polygons.size());

                    // adjacent polygon
                    const traced_polygon_t& next_poly = m0_polygons[m0_next_poly_idx];
                    // pointer to the first halfedge in the polygon from which its stitching will begin
                    const traced_polygon_t::const_iterator he_find_iter = std::find(
                        next_poly.cbegin(),
                        next_poly.cend(),
                        m0_cur_patch_cur_poly_cur_he_opp);

                    // "m0_cur_patch_cur_poly_cur_he_opp" must exist in next_poly since we have
                    // already established that "m0_cur_patch_cur_poly_cur_he" is not a border
                    // halfedge. This is further supported by the fact that "next_poly" is in
                    // current patch and coincident to "m0_cur_patch_cur_poly_cur_he_opp"
                    MCUT_ASSERT(he_find_iter != next_poly.cend());

                    // index of halfedge from which stitching of the adjacent polygon will begin
                    const int m0_next_poly_he_idx = (int)std::distance(next_poly.cbegin(), he_find_iter);

                    // NOTE: there is no need to check if the next polygon is transformed here
                    // because our 4 conditions above take care of this.
                    // However, we do have to take care not to add the polygon to the queue more
                    // than once (due to BFS nature of stitching), hence the following.

                    const bool poly_is_already_in_tmp_queue = std::find_if(
                                                                  patch_poly_stitching_queue_tmp.crbegin(),
                                                                  patch_poly_stitching_queue_tmp.crend(),
                                                                  [&](const std::tuple<hd_t, int, int>& elem) {
                                                                      return std::get<1>(elem) == m0_next_poly_idx;
                                                                  })
                        != patch_poly_stitching_queue_tmp.crend();

                    if (!poly_is_already_in_tmp_queue) {
                        //                   if (!poly_is_already_stitched_wrt_cur_patch) { // TODO: the [if check] will have to go once "poly_is_already_stitched_wrt_cur_patch" is removed
                        // check the main global queue to make sure poly has not already been added
                        // std::unordered_map<int, bool>::const_iterator qmap_iter = m0_poly_already_enqueued.find(m0_next_poly_idx);
                        const bool poly_is_already_in_maqueued = m0_poly_already_enqueued[(std::size_t)m0_next_poly_idx - traced_sm_polygon_count]; /*std::find_if(
                                                                     patch_poly_stitching_queue.crbegin(),
                                                                     patch_poly_stitching_queue.crend(),
                                                                     [&](const std::tuple<hd_t, int, int> &elem)
                                                                     {
                                                                         return std::get<1>(elem) == m0_next_poly_idx; // there is an element in the queue with the polygon's ID
                                                                     }) != patch_poly_stitching_queue.crend();*/

                        if (!poly_is_already_in_maqueued) {
                            patch_poly_stitching_queue_tmp.push_back(std::make_tuple(m1_next_poly_seed_he, m0_next_poly_idx, m0_next_poly_he_idx));
                            m0_poly_already_enqueued[(std::size_t)m0_next_poly_idx - traced_sm_polygon_count] = true;
                        }
                    }

                    //
                    // update vertex mapping for non-intersection points
                    //

                    // since we loop round the whole current polygon, we can just use the target
                    // NOTE: Possible optimization we could populate "m1_to_m0_cm_ovtx_colored" in the
                    // do-while loop that stitches cut-mesh patches because the are specific if-cases which deal
                    // with x-->o and o-->o halfedges, from which we can add elements to "m1_to_m0_cm_ovtx_colored"
                    const vd_t m0_cur_poly_cur_he_tgt = m0.target(m0_cur_patch_cur_poly_cur_he);
                    const bool tgt_is_original_vtx = !m0_is_intersection_point(m0_cur_poly_cur_he_tgt, ps_vtx_cnt);

                    if (tgt_is_original_vtx) {
                        const vd_t m1_cur_poly_cur_he_tgt = m1_colored.target(m1_cur_polygon_he);
                        // "cur_poly_cur_he_tgt" may already be mapped to its "m1" version w.r.t the current patch.
                        // This is because of the BFS manner in which we stitch polygons of a patch.
                        if (m1_to_m0_cm_ovtx_colored.count(m1_cur_poly_cur_he_tgt) == 0) {
                            m1_to_m0_cm_ovtx_colored[m1_cur_poly_cur_he_tgt] = m0_cur_poly_cur_he_tgt;
                        }
                    }
                }
                //              } // for each m0 halfedge of current patch-polygon

                // add elements of tmp/local queue to global queue
                while (!patch_poly_stitching_queue_tmp.empty()) {
                    const std::tuple<hd_t, int, int>& elem = patch_poly_stitching_queue_tmp.front();
                    patch_poly_stitching_queue.push_back(elem); // add
                    patch_poly_stitching_queue_tmp.pop_front(); // rm
                }

                //
                // NOTE: At this stage, we have finished transforming all the halfedges of the current polygon
                // and we have also added all its neighbouring polygons to the queue for stitching.
                //

                MCUT_ASSERT(patch_color_label_to_location.find(color_id) != patch_color_label_to_location.cend());

                ///////////////////////////////////////////////////////////////////////////
                // Update output (with the current polygon stitched into a cc)
                ///////////////////////////////////////////////////////////////////////////

                // const std::string color_tag_stri = to_string(SAFE_ACCESS(patch_color_label_to_location, color_id)); // == cm_patch_location_t::OUTSIDE ? "e" : "i");

                // save meshes and dump

                if (input.keep_fragments_sealed_inside_exhaustive || input.keep_fragments_sealed_outside_exhaustive) {
                    ///////////////////////////////////////////////////////////////////////////
                    // create the sealed meshes defined by the [current] set of traced polygons
                    ///////////////////////////////////////////////////////////////////////////

                    extract_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                        *input.scheduler,
#endif
                        separated_stitching_CCs,
                        m1_colored,
                        0,
                        m1_polygons_colored,
                        sm_polygons_below_cs,
                        sm_polygons_above_cs,
                        m1_vertex_to_seam_flag,
                        m1_to_m0_sm_ovtx_colored,
                        m1_to_m0_cm_ovtx_colored,
                        m1_to_m0_face_colored,
                        m0_to_ps_vtx,
                        m0_to_ps_face,
                        ps_to_sm_vtx,
                        ps_to_sm_face,
                        ps_to_cm_vtx,
                        ps_to_cm_face,
                        sm_vtx_cnt,
                        sm_face_count,
                        input.populate_vertex_maps,
                        input.populate_face_maps,
                        input.keep_fragments_below_cutmesh,
                        input.keep_fragments_above_cutmesh,
                        input.keep_fragments_partially_cut);
                }

                ++global_cm_poly_stitch_counter;
                stitched_poly_counter++;

            } while (!patch_poly_stitching_queue.empty()); // for each polygon of patch

            //
            // NOTE: At this stage we have finished stitching all polygons of the current patch.
            // So, the current patch has been stitch to a src-mesh fragment
            //

        } // for each patch

    } // for each color

    TIMESTACK_POP(); // &&&&&

    patch_poly_stitching_queue.clear();
    m0_poly_already_enqueued.clear();

    m0_cm_poly_to_patch_idx.clear();
    // m0_ivtx_to_ps_edge.clear(); // free
    m0_polygons.clear();
    m0_h_to_ply.clear();
    m0_to_m1_ihe.clear();
    m1_polygons.clear();
    patches.clear();
    patch_to_seed_interior_ihalfedge_idx.clear();
    patch_to_seed_interior_ihalfedge_idx.clear();
    patch_to_seed_poly_idx.clear();
    color_to_patch.clear();
    sm_interior_cs_border_vertices.clear();
    color_to_m0_to_m1_he_instances.clear();

    //
    // NOTE: At this stage, all patches of the current have been stitched
    //

    bool userWantsFullySealedFragmentsANY = (input.keep_fragments_sealed_inside || input.keep_fragments_sealed_outside);
    bool userWantsEvenPartiallySealedFragmentsANY = (input.keep_fragments_sealed_inside_exhaustive || input.keep_fragments_sealed_outside_exhaustive);

    // if the user wants [only] fully sealed fragment (not partially sealed)
    if (userWantsFullySealedFragmentsANY && //
        !userWantsEvenPartiallySealedFragmentsANY) {
        ///////////////////////////////////////////////////////////////////////////////
        // create the [fully] sealed meshes defined by the final set of traced polygons
        ///////////////////////////////////////////////////////////////////////////////

        for (std::map<char, std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>>::iterator color_to_separated_CCs_iter = color_to_separated_connected_ccsponents.begin();
             color_to_separated_CCs_iter != color_to_separated_connected_ccsponents.end();
             ++color_to_separated_CCs_iter) {

            const char color_label = color_to_separated_CCs_iter->first;
            std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>& separated_sealed_CCs = color_to_separated_CCs_iter->second;

            const hmesh_t& m1_colored = SAFE_ACCESS(color_to_m1, color_label);
            MCUT_ASSERT(color_to_m1_polygons.count(color_label) == 1);
            const std::vector<traced_polygon_t>& m1_polygons_colored = SAFE_ACCESS(color_to_m1_polygons, color_label);
            MCUT_ASSERT(color_to_m1_to_m0_sm_ovtx.count(color_label) == 1);
            const std::vector<vd_t>& m1_to_m0_sm_ovtx_colored = SAFE_ACCESS(color_to_m1_to_m0_sm_ovtx, color_label);

            MCUT_ASSERT(colour_to_m1_to_m0_cm_ovtx.count(color_label) == 1);
            const std::unordered_map<vd_t, vd_t>& m1_to_m0_cm_ovtx_colored = SAFE_ACCESS(colour_to_m1_to_m0_cm_ovtx, color_label);
            MCUT_ASSERT(color_to_m1_to_m0_face.count(color_label) == 1);
            /*const*/ std::unordered_map<int, int>& m1_to_m0_face_colored = SAFE_ACCESS(color_to_m1_to_m0_face, color_label);

            // extract the seam vertices
            extract_connected_components(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
                *input.scheduler,
#endif
                separated_sealed_CCs,
                m1_colored,
                0,
                m1_polygons_colored,
                sm_polygons_below_cs,
                sm_polygons_above_cs,
                m1_vertex_to_seam_flag,
                m1_to_m0_sm_ovtx_colored,
                m1_to_m0_cm_ovtx_colored,
                m1_to_m0_face_colored,
                m0_to_ps_vtx,
                m0_to_ps_face,
                ps_to_sm_vtx,
                ps_to_sm_face,
                ps_to_cm_vtx,
                ps_to_cm_face,
                sm_vtx_cnt,
                sm_face_count,
                input.populate_vertex_maps,
                input.populate_face_maps,
                input.keep_fragments_below_cutmesh,
                input.keep_fragments_above_cutmesh,
                input.keep_fragments_partially_cut);
        }
    }

    sm_polygons_below_cs.clear(); // free
    sm_polygons_above_cs.clear();
    m1_vertex_to_seam_flag.clear();
    color_to_m1.clear();
    color_to_m1_polygons.clear();

    ///////////////////////////////////////////////////////////////////////////
    // save output and finish
    ///////////////////////////////////////////////////////////////////////////

    std::map<sm_frag_location_t, std::map<cm_patch_location_t, std::vector<std::shared_ptr<output_mesh_info_t>>>>& out = output.connected_components;
    int idx = 0;
    for (std::map<char, std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>>::iterator color_to_separated_CCs_iter = color_to_separated_connected_ccsponents.begin();
         color_to_separated_CCs_iter != color_to_separated_connected_ccsponents.end();
         ++color_to_separated_CCs_iter) {

        const char color_label = color_to_separated_CCs_iter->first;
        // inside or outside or undefined
        const cm_patch_location_t patchLocation = SAFE_ACCESS(patch_color_label_to_location, color_label);
        std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>& separated_sealed_CCs = color_to_separated_CCs_iter->second;

        for (std::map<std::size_t, std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>>::iterator cc_iter = separated_sealed_CCs.begin();
             cc_iter != separated_sealed_CCs.end();
             ++cc_iter) {

            // all instances of current connected component (from 0-or-1 stitched cm-polygon to all stitched cm-polygons)
            // NOTE TO SELF: the first instance may have one or [zero] cut-mesh polygons since stitching works on a per-patch-per-polygon basis.
            // I.e. if the 1st stitched cm-polygon is into an "above" fragmetn (inside or outside), then the opposite
            // fragment that is "below" will have zero cut-mesh polygons. Hence.
            std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>& cc_instances = cc_iter->second;

            if (!userWantsEvenPartiallySealedFragmentsANY) {
                MCUT_ASSERT(cc_instances.size() == 1); // there is only one, fully sealed, copy
            }

            // For each instance of CC (each instance differs by one stitched polygon)
            for (std::vector<std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>>::iterator cc_instance_iter = cc_instances.begin();
                 cc_instance_iter != cc_instances.end();
                 ++cc_instance_iter) {

                std::pair<std::shared_ptr<hmesh_t>, connected_component_info_t>& cc_instance = *cc_instance_iter;

                if (input.verbose) {
                    // const int idx = (int)std::distance(cc_instances.begin(), cc_instance_iter);
                    dump_mesh(cc_instance.first.get()[0], (std::string("cc") + std::to_string(idx++) + "." + to_string(cc_instance.second.location) + "." + to_string(patchLocation)).c_str());
                }

                std::shared_ptr<output_mesh_info_t> omi = std::shared_ptr<output_mesh_info_t>(new output_mesh_info_t);
                omi->mesh = (cc_instance.first);
                omi->seam_vertices = std::move(cc_instance.second.seam_vertices);
                omi->data_maps = std::move(cc_instance.second.data_maps);
                out[cc_instance.second.location][patchLocation].emplace_back(std::move(omi));
            }
        }
    }

    patch_color_label_to_location.clear(); // free
    color_to_separated_connected_ccsponents.clear();

    TIMESTACK_POP();

    return;
} // dispatch

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

#include <mcut/internal/bvh.h>
#include <mcut/internal/utils.h>
#include "mcut/internal/timer.h"

#include <cmath> // see: if it is possible to remove thsi header
#include <queue>

#ifdef _MSC_VER
#include <intrin.h>
#define __builtin_popcount __popcnt

// https://stackoverflow.com/questions/355967/how-to-use-msvc-intrinsics-to-get-the-equivalent-of-this-gcc-code
unsigned int __inline clz_(unsigned int value)
{
    unsigned long leading_zero = 0;

    if (_BitScanReverse(&leading_zero, value)) {
        return 31 - leading_zero;
    } else {
        // Same remarks as above
        return 32;
    }
}

#endif

#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

#if defined(USE_OIBVH)
// count leading zeros in 32 bit bitfield
unsigned int clz(unsigned int x) // stub
{
#ifdef _MSC_VER
    return clz_(x);
#else
    return __builtin_clz(x); // only tested with gcc!!!
#endif
}

// next power of two from x
int next_power_of_two(int x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
    return x;
}

// check if "x" is a power of two
bool is_power_of_two(int x)
{
    return (x != 0) && !(x & (x - 1));
}

// compute log-base-2 of "x"
int ilog2(unsigned int x)
{
    return sizeof(unsigned int) * CHAR_BIT - clz(x) - 1;
}

// compute index (0...N-1) of the leaf level from the number of leaves
int get_leaf_level_from_real_leaf_count(const int t)
{
    const int np2 = next_power_of_two(t); // todo
    const int tLeafLev = ilog2(np2);
    return tLeafLev;
}

// compute tree-level index from implicit index of a node
int get_level_from_implicit_idx(const int bvhNodeImplicitIndex)
{
    return ilog2(bvhNodeImplicitIndex + 1);
}

// compute previous power of two
unsigned int flp2(unsigned int x) // prev pow2
{
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >> 16);
    return x - (x >> 1);
}

// compute size of of Oi-BVH give number of triangles
int get_ostensibly_implicit_bvh_size(const int t)
{
    return 2 * t - 1 + __builtin_popcount(next_power_of_two(t) - t);
}

// compute left-most node on a given level
int get_level_leftmost_node(const int node_level)
{
    return (1 << node_level) - 1;
}

// compute right-most leaf node in tree
int get_rightmost_real_leaf(const int bvhLeafLevelIndex, const int num_real_leaf_nodes_in_bvh)
{
    return (get_level_leftmost_node(bvhLeafLevelIndex) + num_real_leaf_nodes_in_bvh) - 1;
}

// check if node is a "real node"
bool is_real_implicit_tree_node_id(const int bvhNodeImplicitIndex, const int num_real_leaf_nodes_in_bvh)
{

    const int t = num_real_leaf_nodes_in_bvh;
    // const int q = bvhNodeImplicitIndex; // queried node
    const int li = get_leaf_level_from_real_leaf_count(t);
    const int i = get_rightmost_real_leaf(li, t);
    const int lq = get_level_from_implicit_idx(bvhNodeImplicitIndex);
    const int p = (int)((1.0f / (1 << (li - lq))) + ((float)i / (1 << (li - lq))) - 1);

    return bvhNodeImplicitIndex <= p || p == 0; // and p is not the root
}

// get the right most real node on a given tree level
int get_level_rightmost_real_node(
    const int rightmostRealLeafNodeImplicitIndex,
    const int bvhLeafLevelIndex,
    const int ancestorLevelIndex)
{
    using namespace std;
    const int level_dist = (bvhLeafLevelIndex - ancestorLevelIndex);
    const int implicit_index_of_ancestor = (int)((1.0f / (1 << level_dist)) + ((float)rightmostRealLeafNodeImplicitIndex / (1 << level_dist)) - 1);
    return implicit_index_of_ancestor;
}

// compute implicit index of a node's ancestor
int get_node_ancestor(
    const int nodeImplicitIndex,
    const int nodeLevelIndex,
    const int ancestorLevelIndex)
{
    using namespace std;
    const int levelDistance = nodeLevelIndex - ancestorLevelIndex;
    return (int)((1.0f / (1 << levelDistance)) + ((float)nodeImplicitIndex / (1 << levelDistance)) - 1); /*trunc((1.0f / pow(bvhDegree, level_dist)) + (rightmostRealLeafNodeImplicitIndex / pow(bvhDegree, level_dist)) - 1)*/
}

// calculate linear memory index of a real node
int get_node_mem_index(
    const int nodeImplicitIndex,
    const int leftmostImplicitIndexOnNodeLevel,
    const int bvh_data_base_offset,
    const int rightmostRealNodeImplicitIndexOnNodeLevel)
{
    return bvh_data_base_offset + get_ostensibly_implicit_bvh_size((rightmostRealNodeImplicitIndexOnNodeLevel - leftmostImplicitIndexOnNodeLevel) + 1) - 1 - (rightmostRealNodeImplicitIndexOnNodeLevel - nodeImplicitIndex);
}

// Expands a 10-bit integer into 30 bits by inserting 2 zeros after each bit.
unsigned int expandBits(unsigned int v)
{
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
};

// Calculates a 30-bit Morton code for the given 3D point located within the unit cube [0,1].
unsigned int morton3D(float x, float y, float z)
{
    x = std::fmin(std::fmax(x * 1024.0f, 0.0f), 1023.0f);
    y = std::fmin(std::fmax(y * 1024.0f, 0.0f), 1023.0f);
    z = std::fmin(std::fmax(z * 1024.0f, 0.0f), 1023.0f);

    unsigned int xx = expandBits((unsigned int)x);
    unsigned int yy = expandBits((unsigned int)y);
    unsigned int zz = expandBits((unsigned int)z);

    return (xx * 4 + yy * 2 + zz);
};

void build_oibvh(
    #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    thread_pool& pool,
    #endif
    const hmesh_t& mesh,
    std::vector<bounding_box_t<vec3>>& bvhAABBs,
    std::vector<fd_t>& bvhLeafNodeFaces,
    std::vector<bounding_box_t<vec3>>& face_bboxes,
    const double& slightEnlargmentEps)
{
    SCOPED_TIMER(__FUNCTION__);

    const int meshFaceCount = mesh.number_of_faces();
    const int bvhNodeCount = get_ostensibly_implicit_bvh_size(meshFaceCount);

    // compute mesh-face bounding boxes and their centers
    // ::::::::::::::::::::::::::::::::::::::::::::::::::

    face_bboxes.resize(meshFaceCount); //, bounding_box_t<vec3>());
    std::vector<vec3> face_bbox_centers(meshFaceCount, vec3());
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        auto fn_compute_face_bbox_data = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
            for (face_array_iterator_t f = block_start_; f != block_end_; ++f) {
                const int faceIdx = static_cast<int>(*f);
                std::vector<vd_t> vertices_on_face = mesh.get_vertices_around_face(*f);

                // for each vertex on face
                for (std::vector<vd_t>::const_iterator v = vertices_on_face.cbegin(); v != vertices_on_face.cend(); ++v) {
                    const vec3 coords = mesh.vertex(*v);
                    face_bboxes[faceIdx].expand(coords);
                }

                bounding_box_t<vec3>& bbox = face_bboxes[faceIdx];

                if (slightEnlargmentEps > double(0.0)) {
                    bbox.enlarge(slightEnlargmentEps);
                }

                // calculate bbox center
                face_bbox_centers[*f] = (bbox.minimum() + bbox.maximum()) / 2;
            }
        };

        parallel_for(
            pool,
            mesh.faces_begin(),
            mesh.faces_end(),
            fn_compute_face_bbox_data);
    }
#else

    // for each face in mesh
    for (face_array_iterator_t f = mesh.faces_begin(); f != mesh.faces_end(); ++f) {
        const int faceIdx = static_cast<int>(*f);
        const std::vector<vd_t> vertices_on_face = mesh.get_vertices_around_face(*f);

        // for each vertex on face
        for (std::vector<vd_t>::const_iterator v = vertices_on_face.cbegin(); v != vertices_on_face.cend(); ++v) {
            const vec3 coords = mesh.vertex(*v);
            face_bboxes[faceIdx].expand(coords);
        }

        bounding_box_t<vec3>& bbox = face_bboxes[faceIdx];

        if (slightEnlargmentEps > double(0.0)) {
            bbox.enlarge(slightEnlargmentEps);
        }

        // calculate bbox center
        face_bbox_centers[*f] = (bbox.minimum() + bbox.maximum()) / 2;
    }
#endif
    // compute mesh bounding box
    // :::::::::::::::::::::::::

    bvhAABBs.resize(bvhNodeCount);
    bounding_box_t<vec3>& meshBbox = bvhAABBs.front(); // root bounding box

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        std::mutex bbox_expansion_mtx;
        auto fn_compute_mesh_bbox = [&](vertex_array_iterator_t block_start_, vertex_array_iterator_t block_end_) {
            bounding_box_t<vec3> meshBbox_local;
            for (vertex_array_iterator_t v = block_start_; v != block_end_; ++v) {
                const vec3& coords = mesh.vertex(*v);
                meshBbox_local.expand(coords);
            }

            std::lock_guard<std::mutex> lock(bbox_expansion_mtx);
            meshBbox.expand(meshBbox_local);
        };

        parallel_for(
            pool,
            mesh.vertices_begin(),
            mesh.vertices_end(),
            fn_compute_mesh_bbox);
    }
#else
    // for each vertex in mesh
    for (vertex_array_iterator_t v = mesh.vertices_begin(); v != mesh.vertices_end(); ++v) {
        const vec3& coords = mesh.vertex(*v);
        meshBbox.expand(coords);
    }
#endif

    // compute morton codes
    // ::::::::::::::::::::

    std::vector<std::pair<fd_t, uint32_t>> bvhLeafNodeDescriptors(meshFaceCount, std::pair<fd_t, uint32_t>());

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        auto fn_compute_morton_codes = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
            for (face_array_iterator_t f = block_start_; f != block_end_; ++f) {
                const uint32_t faceIdx = static_cast<uint32_t>(*f);

                const vec3& face_aabb_centre = SAFE_ACCESS(face_bbox_centers, faceIdx);
                const vec3 offset = face_aabb_centre - meshBbox.minimum();
                const vec3 dims = meshBbox.maximum() - meshBbox.minimum();

                const unsigned int mortion_code = morton3D(
                    static_cast<float>(offset.x() / dims.x()),
                    static_cast<float>(offset.y() / dims.y()),
                    static_cast<float>(offset.z() / dims.z()));

                const uint32_t idx = (uint32_t)std::distance(mesh.faces_begin(), f); // NOTE: mesh.faces_begin() may not be the actual beginning internally
                bvhLeafNodeDescriptors[idx].first = *f;
                bvhLeafNodeDescriptors[idx].second = mortion_code;
            }
        };

        parallel_for(
            pool,
            mesh.faces_begin(),
            mesh.faces_end(),
            fn_compute_morton_codes);
    }
#else
    for (face_array_iterator_t f = mesh.faces_begin(); f != mesh.faces_end(); ++f) {
        const uint32_t faceIdx = static_cast<uint32_t>(*f);

        const vec3& face_aabb_centre = SAFE_ACCESS(face_bbox_centers, faceIdx);
        const vec3 offset = face_aabb_centre - meshBbox.minimum();
        const vec3 dims = meshBbox.maximum() - meshBbox.minimum();

        const unsigned int mortion_code = morton3D(
            static_cast<float>(offset.x() / dims.x()),
            static_cast<float>(offset.y() / dims.y()),
            static_cast<float>(offset.z() / dims.z()));

        const uint32_t idx = (uint32_t)std::distance(mesh.faces_begin(), f); // NOTE: mesh.faces_begin() may not be the actual beginning internally
        bvhLeafNodeDescriptors[idx].first = *f;
        bvhLeafNodeDescriptors[idx].second = mortion_code;
    }
#endif
    // sort faces according to morton codes

    // TODO: make parallel
    std::sort(
        bvhLeafNodeDescriptors.begin(),
        bvhLeafNodeDescriptors.end(),
        [](const std::pair<fd_t, uint32_t>& a, const std::pair<fd_t, uint32_t>& b) {
            return a.second < b.second;
        });

    bvhLeafNodeFaces.resize(meshFaceCount);

    const int leaf_level_index = get_leaf_level_from_real_leaf_count(meshFaceCount);
    const int leftmost_real_node_on_leaf_level = get_level_leftmost_node(leaf_level_index);
    const int rightmost_real_leaf = get_rightmost_real_leaf(leaf_level_index, meshFaceCount);
    const int rightmost_real_node_on_leaf_level = get_level_rightmost_real_node(rightmost_real_leaf, leaf_level_index, leaf_level_index);
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        auto fn_save_leaf_node_info = [&](std::vector<std::pair<fd_t, uint32_t>>::const_iterator block_start_, std::vector<std::pair<fd_t, uint32_t>>::const_iterator block_end_) {
            for (std::vector<std::pair<fd_t, uint32_t>>::const_iterator it = block_start_; it != block_end_; ++it) {
                const uint32_t index_on_leaf_level = (uint32_t)std::distance(bvhLeafNodeDescriptors.cbegin(), it);

                bvhLeafNodeFaces[index_on_leaf_level] = it->first;

                const int implicit_idx = leftmost_real_node_on_leaf_level + index_on_leaf_level;
                const int memory_idx = get_node_mem_index(
                    implicit_idx,
                    leftmost_real_node_on_leaf_level,
                    0,
                    rightmost_real_node_on_leaf_level);

                const bounding_box_t<vec3>& face_bbox = face_bboxes[(uint32_t)it->first];
                bvhAABBs[memory_idx] = face_bbox;
            }
        };

        parallel_for(
            pool,
            bvhLeafNodeDescriptors.cbegin(),
            bvhLeafNodeDescriptors.cend(),
            fn_save_leaf_node_info);
    }
#else
    // save sorted leaf node bvhAABBs and their corrresponding face id
    for (std::vector<std::pair<fd_t, uint32_t>>::const_iterator it = bvhLeafNodeDescriptors.cbegin(); it != bvhLeafNodeDescriptors.cend(); ++it) {
        const uint32_t index_on_leaf_level = (uint32_t)std::distance(bvhLeafNodeDescriptors.cbegin(), it);

        bvhLeafNodeFaces[index_on_leaf_level] = it->first;

        const int implicit_idx = leftmost_real_node_on_leaf_level + index_on_leaf_level;
        const int memory_idx = get_node_mem_index(
            implicit_idx,
            leftmost_real_node_on_leaf_level,
            0,
            rightmost_real_node_on_leaf_level);

        const bounding_box_t<vec3>& face_bbox = face_bboxes[(uint32_t)it->first];
        bvhAABBs[memory_idx] = face_bbox;
    }
#endif
    // construct internal-node bounding boxes
    // ::::::::::::::::::::::::::::::::::::::

    // for each level in the oi-bvh tree (starting from the penultimate level)
    for (int level_index = leaf_level_index - 1; level_index >= 0; --level_index) {

        const int rightmost_real_node_on_level = get_level_rightmost_real_node(rightmost_real_leaf, leaf_level_index, level_index);
        const int leftmost_real_node_on_level = get_level_leftmost_node(level_index);
        const int number_of_real_nodes_on_level = (rightmost_real_node_on_level - leftmost_real_node_on_level) + 1;

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        {
            // allows us to pretend that we can use an iterator over the nodes
            std::vector<uint8_t> level_nodes_placeholder(number_of_real_nodes_on_level);
            auto fn_compute_bvh_level_nodes = [&](std::vector<uint8_t>::const_iterator block_start_, std::vector<uint8_t>::const_iterator block_end_) {
                uint32_t base_offset = std::distance(level_nodes_placeholder.cbegin(), block_start_);
                uint32_t counter = 0;
                for (std::vector<uint8_t>::const_iterator it = block_start_; it != block_end_; ++it) {
                    const int level_node_idx_iter = base_offset + (counter++);

                    const int node_implicit_idx = leftmost_real_node_on_level + level_node_idx_iter;
                    const int left_child_implicit_idx = (node_implicit_idx * 2) + 1;
                    const int right_child_implicit_idx = (node_implicit_idx * 2) + 2;
                    const bool is_penultimate_level = (level_index == (leaf_level_index - 1));
                    const int rightmost_real_node_on_child_level = get_level_rightmost_real_node(rightmost_real_leaf, leaf_level_index, level_index + 1);
                    const int leftmost_real_node_on_child_level = get_level_leftmost_node(level_index + 1);
                    const bool right_child_exists = (right_child_implicit_idx <= rightmost_real_node_on_child_level);

                    bounding_box_t<vec3> node_bbox;

                    if (is_penultimate_level) { // both children are leaves

                        const int left_child_index_on_level = left_child_implicit_idx - leftmost_real_node_on_child_level;
                        const fd_t& left_child_face = SAFE_ACCESS(bvhLeafNodeFaces, left_child_index_on_level);
                        const bounding_box_t<vec3>& left_child_bbox = SAFE_ACCESS(face_bboxes, left_child_face);

                        node_bbox.expand(left_child_bbox);

                        if (right_child_exists) {
                            const int right_child_index_on_level = right_child_implicit_idx - leftmost_real_node_on_child_level;
                            const fd_t& right_child_face = SAFE_ACCESS(bvhLeafNodeFaces, right_child_index_on_level);
                            const bounding_box_t<vec3>& right_child_bbox = SAFE_ACCESS(face_bboxes, right_child_face);
                            node_bbox.expand(right_child_bbox);
                        }
                    } else { // remaining internal node levels

                        const int left_child_memory_idx = get_node_mem_index(
                            left_child_implicit_idx,
                            leftmost_real_node_on_child_level,
                            0,
                            rightmost_real_node_on_child_level);
                        const bounding_box_t<vec3>& left_child_bbox = SAFE_ACCESS(bvhAABBs, left_child_memory_idx);

                        node_bbox.expand(left_child_bbox);

                        if (right_child_exists) {
                            const int right_child_memory_idx = get_node_mem_index(
                                right_child_implicit_idx,
                                leftmost_real_node_on_child_level,
                                0,
                                rightmost_real_node_on_child_level);
                            const bounding_box_t<vec3>& right_child_bbox = SAFE_ACCESS(bvhAABBs, right_child_memory_idx);
                            node_bbox.expand(right_child_bbox);
                        }
                    }

                    const int node_memory_idx = get_node_mem_index(
                        node_implicit_idx,
                        leftmost_real_node_on_level,
                        0,
                        rightmost_real_node_on_level);

                    SAFE_ACCESS(bvhAABBs, node_memory_idx) = node_bbox;
                }
            };

            parallel_for(
                pool,
                level_nodes_placeholder.cbegin(),
                level_nodes_placeholder.cend(),
                fn_compute_bvh_level_nodes);
        }
#else
        // for each node on the current level
        for (int level_node_idx_iter = 0; level_node_idx_iter < number_of_real_nodes_on_level; ++level_node_idx_iter) {

            const int node_implicit_idx = leftmost_real_node_on_level + level_node_idx_iter;
            const int left_child_implicit_idx = (node_implicit_idx * 2) + 1;
            const int right_child_implicit_idx = (node_implicit_idx * 2) + 2;
            const bool is_penultimate_level = (level_index == (leaf_level_index - 1));
            const int rightmost_real_node_on_child_level = get_level_rightmost_real_node(rightmost_real_leaf, leaf_level_index, level_index + 1);
            const int leftmost_real_node_on_child_level = get_level_leftmost_node(level_index + 1);
            const bool right_child_exists = (right_child_implicit_idx <= rightmost_real_node_on_child_level);

            bounding_box_t<vec3> node_bbox;

            if (is_penultimate_level) { // both children are leaves

                const int left_child_index_on_level = left_child_implicit_idx - leftmost_real_node_on_child_level;
                const fd_t& left_child_face = SAFE_ACCESS(bvhLeafNodeFaces, left_child_index_on_level);
                const bounding_box_t<vec3>& left_child_bbox = SAFE_ACCESS(face_bboxes, left_child_face);

                node_bbox.expand(left_child_bbox);

                if (right_child_exists) {
                    const int right_child_index_on_level = right_child_implicit_idx - leftmost_real_node_on_child_level;
                    const fd_t& right_child_face = SAFE_ACCESS(bvhLeafNodeFaces, right_child_index_on_level);
                    const bounding_box_t<vec3>& right_child_bbox = SAFE_ACCESS(face_bboxes, right_child_face);
                    node_bbox.expand(right_child_bbox);
                }
            } else { // remaining internal node levels

                const int left_child_memory_idx = get_node_mem_index(
                    left_child_implicit_idx,
                    leftmost_real_node_on_child_level,
                    0,
                    rightmost_real_node_on_child_level);
                const bounding_box_t<vec3>& left_child_bbox = SAFE_ACCESS(bvhAABBs, left_child_memory_idx);

                node_bbox.expand(left_child_bbox);

                if (right_child_exists) {
                    const int right_child_memory_idx = get_node_mem_index(
                        right_child_implicit_idx,
                        leftmost_real_node_on_child_level,
                        0,
                        rightmost_real_node_on_child_level);
                    const bounding_box_t<vec3>& right_child_bbox = SAFE_ACCESS(bvhAABBs, right_child_memory_idx);
                    node_bbox.expand(right_child_bbox);
                }
            }

            const int node_memory_idx = get_node_mem_index(
                node_implicit_idx,
                leftmost_real_node_on_level,
                0,
                rightmost_real_node_on_level);

            SAFE_ACCESS(bvhAABBs, node_memory_idx) = node_bbox;
        } // for each real node on level
#endif
    } // for each internal level
}

void intersectOIBVHs(
    std::map<fd_t, std::vector<fd_t>>& ps_face_to_potentially_intersecting_others,
    const std::vector<bounding_box_t<vec3>>& srcMeshBvhAABBs,
    const std::vector<fd_t>& srcMeshBvhLeafNodeFaces,
    const std::vector<bounding_box_t<vec3>>& cutMeshBvhAABBs,
    const std::vector<fd_t>& cutMeshBvhLeafNodeFaces)
{
    TIMESTACK_PUSH(__FUNCTION__);
    // simultaneuosly traverse both BVHs to find intersecting pairs
    std::queue<node_pair_t> traversalQueue;
    traversalQueue.push({ 0, 0 }); // left = sm BVH; right = cm BVH

    const int numSrcMeshFaces = (int)srcMeshBvhLeafNodeFaces.size();
    MCUT_ASSERT(numSrcMeshFaces >= 1);
    const int numCutMeshFaces = (int)cutMeshBvhLeafNodeFaces.size();
    MCUT_ASSERT(numCutMeshFaces >= 1);

    const int sm_bvh_leaf_level_idx = get_leaf_level_from_real_leaf_count(numSrcMeshFaces);
    const int cs_bvh_leaf_level_idx = get_leaf_level_from_real_leaf_count(numCutMeshFaces);

    const int sm_bvh_rightmost_real_leaf = get_rightmost_real_leaf(sm_bvh_leaf_level_idx, numSrcMeshFaces);
    const int cs_bvh_rightmost_real_leaf = get_rightmost_real_leaf(cs_bvh_leaf_level_idx, numCutMeshFaces);

    do {
        node_pair_t ct_front_node = traversalQueue.front();

        bounding_box_t<vec3> sm_bvh_node_bbox;
        bounding_box_t<vec3> cs_bvh_node_bbox;

        // sm
        const int sm_bvh_node_implicit_idx = ct_front_node.m_left;
        const int sm_bvh_node_level_idx = get_level_from_implicit_idx(sm_bvh_node_implicit_idx);
        const bool sm_bvh_node_is_leaf = sm_bvh_node_level_idx == sm_bvh_leaf_level_idx;
        const int sm_bvh_node_level_leftmost_node = get_level_leftmost_node(sm_bvh_node_level_idx);
        fd_t sm_node_face = hmesh_t::null_face();
        const int sm_bvh_node_level_rightmost_node = get_level_rightmost_real_node(sm_bvh_rightmost_real_leaf, sm_bvh_leaf_level_idx, sm_bvh_node_level_idx);
        const int sm_bvh_node_mem_idx = get_node_mem_index(
            sm_bvh_node_implicit_idx,
            sm_bvh_node_level_leftmost_node,
            0,
            sm_bvh_node_level_rightmost_node);
        sm_bvh_node_bbox = SAFE_ACCESS(srcMeshBvhAABBs, sm_bvh_node_mem_idx);

        if (sm_bvh_node_is_leaf) {
            const int sm_bvh_node_idx_on_level = sm_bvh_node_implicit_idx - sm_bvh_node_level_leftmost_node;
            sm_node_face = SAFE_ACCESS(srcMeshBvhLeafNodeFaces, sm_bvh_node_idx_on_level);
        }

        // cs
        const int cs_bvh_node_implicit_idx = ct_front_node.m_right;
        const int cs_bvh_node_level_idx = get_level_from_implicit_idx(cs_bvh_node_implicit_idx);
        const int cs_bvh_node_level_leftmost_node = get_level_leftmost_node(cs_bvh_node_level_idx);
        const bool cs_bvh_node_is_leaf = cs_bvh_node_level_idx == cs_bvh_leaf_level_idx;
        fd_t cs_node_face = hmesh_t::null_face();
        const int cs_bvh_node_level_rightmost_node = get_level_rightmost_real_node(cs_bvh_rightmost_real_leaf, cs_bvh_leaf_level_idx, cs_bvh_node_level_idx);
        const int cs_bvh_node_mem_idx = get_node_mem_index(
            cs_bvh_node_implicit_idx,
            cs_bvh_node_level_leftmost_node,
            0,
            cs_bvh_node_level_rightmost_node);
        cs_bvh_node_bbox = SAFE_ACCESS(cutMeshBvhAABBs, cs_bvh_node_mem_idx);

        if (cs_bvh_node_is_leaf) {
            const int cs_bvh_node_idx_on_level = cs_bvh_node_implicit_idx - cs_bvh_node_level_leftmost_node;
            cs_node_face = SAFE_ACCESS(cutMeshBvhLeafNodeFaces, cs_bvh_node_idx_on_level);
        }

        const bool haveOverlap = intersect_bounding_boxes(sm_bvh_node_bbox, cs_bvh_node_bbox);

        if (haveOverlap) {

            if (cs_bvh_node_is_leaf && sm_bvh_node_is_leaf) {
                MCUT_ASSERT(cs_node_face != hmesh_t::null_face());
                MCUT_ASSERT(sm_node_face != hmesh_t::null_face());

                fd_t cs_node_face_offsetted = fd_t(cs_node_face + numSrcMeshFaces);

                ps_face_to_potentially_intersecting_others[sm_node_face].push_back(cs_node_face_offsetted);
                ps_face_to_potentially_intersecting_others[cs_node_face_offsetted].push_back(sm_node_face);
            } else if (sm_bvh_node_is_leaf && !cs_bvh_node_is_leaf) {
                MCUT_ASSERT(cs_node_face == hmesh_t::null_face());
                MCUT_ASSERT(sm_node_face != hmesh_t::null_face());

                const int cs_bvh_node_left_child_implicit_idx = (cs_bvh_node_implicit_idx * 2) + 1;
                const int cs_bvh_node_right_child_implicit_idx = (cs_bvh_node_implicit_idx * 2) + 2;

                const int rightmost_real_node_on_child_level = get_level_rightmost_real_node(cs_bvh_rightmost_real_leaf, cs_bvh_leaf_level_idx, cs_bvh_node_level_idx + 1);
                const bool right_child_is_real = cs_bvh_node_right_child_implicit_idx <= rightmost_real_node_on_child_level;

                traversalQueue.push({ sm_bvh_node_implicit_idx, cs_bvh_node_left_child_implicit_idx });

                if (right_child_is_real) {
                    traversalQueue.push({ sm_bvh_node_implicit_idx, cs_bvh_node_right_child_implicit_idx });
                }
            } else if (!sm_bvh_node_is_leaf && cs_bvh_node_is_leaf) {

                MCUT_ASSERT(cs_node_face != hmesh_t::null_face());
                MCUT_ASSERT(sm_node_face == hmesh_t::null_face());

                const int sm_bvh_node_left_child_implicit_idx = (sm_bvh_node_implicit_idx * 2) + 1;
                const int sm_bvh_node_right_child_implicit_idx = (sm_bvh_node_implicit_idx * 2) + 2;

                const int rightmost_real_node_on_child_level = get_level_rightmost_real_node(sm_bvh_rightmost_real_leaf, sm_bvh_leaf_level_idx, sm_bvh_node_level_idx + 1);
                const bool right_child_is_real = sm_bvh_node_right_child_implicit_idx <= rightmost_real_node_on_child_level;

                traversalQueue.push({ sm_bvh_node_left_child_implicit_idx, cs_bvh_node_implicit_idx });

                if (right_child_is_real) {
                    traversalQueue.push({ sm_bvh_node_right_child_implicit_idx, cs_bvh_node_implicit_idx });
                }
            } else { // both nodes are internal
                MCUT_ASSERT(cs_node_face == hmesh_t::null_face());
                MCUT_ASSERT(sm_node_face == hmesh_t::null_face());

                const int sm_bvh_node_left_child_implicit_idx = (sm_bvh_node_implicit_idx * 2) + 1;
                const int sm_bvh_node_right_child_implicit_idx = (sm_bvh_node_implicit_idx * 2) + 2;

                const int cs_bvh_node_left_child_implicit_idx = (cs_bvh_node_implicit_idx * 2) + 1;
                const int cs_bvh_node_right_child_implicit_idx = (cs_bvh_node_implicit_idx * 2) + 2;

                const int sm_rightmost_real_node_on_child_level = get_level_rightmost_real_node(sm_bvh_rightmost_real_leaf, sm_bvh_leaf_level_idx, sm_bvh_node_level_idx + 1);
                const bool sm_right_child_is_real = sm_bvh_node_right_child_implicit_idx <= sm_rightmost_real_node_on_child_level;

                const int cs_rightmost_real_node_on_child_level = get_level_rightmost_real_node(cs_bvh_rightmost_real_leaf, cs_bvh_leaf_level_idx, cs_bvh_node_level_idx + 1);
                const bool cs_right_child_is_real = cs_bvh_node_right_child_implicit_idx <= cs_rightmost_real_node_on_child_level;

                traversalQueue.push({ sm_bvh_node_left_child_implicit_idx, cs_bvh_node_left_child_implicit_idx });

                if (cs_right_child_is_real) {
                    traversalQueue.push({ sm_bvh_node_left_child_implicit_idx, cs_bvh_node_right_child_implicit_idx });
                }

                if (sm_right_child_is_real) {
                    traversalQueue.push({ sm_bvh_node_right_child_implicit_idx, cs_bvh_node_left_child_implicit_idx });

                    if (cs_right_child_is_real) {
                        traversalQueue.push({ sm_bvh_node_right_child_implicit_idx, cs_bvh_node_right_child_implicit_idx });
                    }
                }
            }
        }

        traversalQueue.pop(); // rm ct_front_node
    } while (!traversalQueue.empty());
    TIMESTACK_POP();
}
#else
BoundingVolumeHierarchy::BoundingVolumeHierarchy()
{
}

BoundingVolumeHierarchy::~BoundingVolumeHierarchy() { }

// three stages to BVH construction
void BoundingVolumeHierarchy::buildTree(const hmesh_t& mesh_,
    const fixed_precision_number_t& enlargementEps_,
    uint32_t mp_,
    const SplitMethod& sm_)
{
    SCOPED_TIMER(__FUNCTION__);
    mesh = &(mesh_); ///
    MCUT_ASSERT(mesh->number_of_faces() >= 1);
    maxPrimsInNode = (std::min(255u, mp_)); //
    splitMethod = (sm_); //
    enlargementEps = (enlargementEps_);

    buildData.clear();
    primitives.clear();
    primitiveOrderedBBoxes.clear();
    nodes.clear();

    // First, bounding information about each primitive is computed and stored in an array
    // that will be used during tree construction.

    // initialize buildData array for primitives

    buildData.reserve(mesh->number_of_faces());

    primitiveOrderedBBoxes.resize(mesh->number_of_faces());
    primitives.resize(mesh->number_of_faces());

    // TODO make this parallel
    // for each face in mesh
    for (face_array_iterator_t f = mesh->faces_begin(); f != mesh->faces_end(); ++f) {
        const int i = static_cast<int>(*f);
        primitives[i] = *f;

        const std::vector<vd_t> vertices_on_face = mesh->get_vertices_around_face(*f);

        bounding_box_t<vec3> bbox;
        // for each vertex on face
        for (std::vector<vd_t>::const_iterator v = vertices_on_face.cbegin(); v != vertices_on_face.cend(); ++v) {
            const vec3 coords = mesh->vertex(*v);
            bbox.expand(coords);
        }

        if (enlargementEps > 0.0) {
            bbox.enlarge(enlargementEps);
        }

        primitiveOrderedBBoxes[i] = bbox;

        buildData.push_back(BVHPrimitiveInfo(i, primitiveOrderedBBoxes[i]));
    }

    // Next, the tree is built via a procedure that splits the primitives into subsets and
    // recursively builds BVHs for the subsets. The result is a binary tree where each
    // interior node holds pointers to its children and each leaf node holds references to
    // one or more primitives.

    uint32_t totalNodes = 0;
    std::vector<fd_t> orderedPrims;
    // orderedPrims.reserve(mesh->number_of_faces());

    std::shared_ptr<BVHBuildNode> root = recursiveBuild(
        buildData,
        0,
        /*primitives.size()*/ mesh->number_of_faces(),
        &totalNodes,
        orderedPrims);

    primitives.swap(orderedPrims);

    // Finally, this tree is converted to a more compact (and thus more efficient) pointerless
    // representation for use during rendering

    nodes.resize(totalNodes);
    for (uint32_t i = 0; i < totalNodes; ++i) {
        // new (&nodes[i]) LinearBVHNode;
        nodes[i] = std::make_shared<LinearBVHNode>();
    }
    uint32_t offset = 0;
    flattenBVHTree(root, &offset);
}

const BBox& BoundingVolumeHierarchy::GetPrimitiveBBox(int primitiveIndex) const
{
    MCUT_ASSERT(primitiveIndex < mesh->number_of_faces());
    return primitiveOrderedBBoxes[primitiveIndex];
}

uint32_t BoundingVolumeHierarchy::flattenBVHTree(std::shared_ptr<BVHBuildNode> node, uint32_t* offset)
{
    MCUT_ASSERT(*offset < nodes.size());
    std::shared_ptr<LinearBVHNode> linearNode = nodes[*offset];
    linearNode->bounds = node->bounds;
    uint32_t myOffset = (*offset)++;
    if (node->nPrimitives > 0) {
        linearNode->primitivesOffset = node->firstPrimOffset;
        linearNode->nPrimitives = node->nPrimitives;
    } else {
        // Creater interior flattened BVH node
        linearNode->axis = node->splitAxis;
        linearNode->nPrimitives = 0;

        flattenBVHTree(node->children[0], offset);

        linearNode->secondChildOffset = flattenBVHTree(node->children[1],
            offset);
    }
    return myOffset;
}

std::shared_ptr<BVHBuildNode> BoundingVolumeHierarchy::recursiveBuild(
    std::vector<BVHPrimitiveInfo>& buildData,
    uint32_t start,
    uint32_t end,
    uint32_t* totalNodes,
    std::vector<fd_t>& orderedPrims)
{
    (*totalNodes)++;

    std::shared_ptr<BVHBuildNode> node = std::make_shared<BVHBuildNode>();

    // Compute bounds of all primitives in BVH node
    uint32_t nPrimitives = end - start;
    MCUT_ASSERT((nPrimitives - 1) < (uint32_t)mesh->number_of_faces());

    BBox bbox;
    for (uint32_t i = start; i < end; ++i) {
        MCUT_ASSERT(i < buildData.size());
        bbox = Union(bbox, buildData[i].bounds);
    }
    if (nPrimitives == 1) {
        // Create leaf BVHBuildNode
        uint32_t firstPrimOffset = orderedPrims.size();
        for (uint32_t i = start; i < end; ++i) {
            MCUT_ASSERT(i < buildData.size());
            uint32_t primNum = buildData[i].primitiveNumber;
            orderedPrims.push_back(primitives[primNum]);
        }
        node->InitLeaf(firstPrimOffset, nPrimitives, bbox);
    } else {
        // Compute bound of primitive centroids, choose split dimension dim
        BBox centroidBounds;
        for (uint32_t i = start; i < end; ++i) {
            MCUT_ASSERT(i < buildData.size());
            centroidBounds = Union(centroidBounds, buildData[i].centroid);
        }

        int dim = centroidBounds.MaximumExtent();
        MCUT_ASSERT(dim < 3);

        //
        // Partition primitives into two sets and build children
        //
        uint32_t mid = (start + end) / 2;
        switch (this->splitMethod) {
        case SplitMethod::SPLIT_MIDDLE: {
            // Partition primitives through node’s midpoint
            fixed_precision_number_t pmid = (centroidBounds.minimum()[dim] + centroidBounds.maximum()[dim]) * .5;
#if 1
            MCUT_ASSERT(start < buildData.size());
            BVHPrimitiveInfo* midPtr = std::partition(&buildData[start],
                &buildData[end - 1] + 1,
                /*CompareToMid(dim, pmid)*/
                [dim, pmid](const BVHPrimitiveInfo& pi) {
                    return pi.centroid[dim] < pmid;
                });
            mid = midPtr - &buildData[0];
#else
            std::vector<BVHPrimitiveInfo>::iterator midPtr = std::partition(buildData.begin() + start,
                buildData.end(),
                CompareToMid(dim, pmid));
            mid = std::distance(buildData.begin(), midPtr);
#endif
        } break;
        case SplitMethod::SPLIT_EQUAL_COUNTS: {
            // Partition primitives into equally-sized subsets
            mid = (start + end) / 2;
            std::nth_element(&buildData[start], &buildData[mid],
                &buildData[end - 1] + 1, ComparePoints(dim));
        } break;
        case SplitMethod::SPLIT_SAH: {
            // Partition primitives using approximate SAH
            if (nPrimitives <= 4) {
                // Partition primitives into equally-sized subsets
                mid = (start + end) / 2;
                std::nth_element(&buildData[start], &buildData[mid],
                    &buildData[end - 1] + 1, ComparePoints(dim));
            } else {
                // Allocate BucketInfo for SAH partition buckets
                const int nBuckets = 12;
                BucketInfo buckets[nBuckets];

                // Initialize BucketInfo for SAH partition buckets
                for (uint32_t i = start; i < end; ++i) {
                    int b = nBuckets * ((buildData[i].centroid[dim] - centroidBounds.minimum()[dim]) / (centroidBounds.maximum()[dim] - centroidBounds.minimum()[dim]));
                    if (b == nBuckets)
                        b = nBuckets - 1;
                    buckets[b].count++;
                    buckets[b].bounds = Union(buckets[b].bounds, buildData[i].bounds);
                }
                // Compute costs for splitting after each bucket

                fixed_precision_number_t cost[nBuckets - 1];

                for (int i = 0; i < nBuckets - 1; ++i) {
                    BBox b0, b1;
                    int count0 = 0, count1 = 0;
                    for (int j = 0; j <= i; ++j) {
                        b0 = Union(b0, buckets[j].bounds);
                        count0 += buckets[j].count;
                    }
                    for (int j = i + 1; j < nBuckets; ++j) {
                        b1 = Union(b1, buckets[j].bounds);
                        count1 += buckets[j].count;
                    }
                    cost[i] = .125f + (count0 * b0.SurfaceArea() + count1 * b1.SurfaceArea()) / bbox.SurfaceArea();
                }
                // Find bucket to split at that minimizes SAH metric
                float minCost = cost[0];
                uint32_t minCostSplit = 0;
                for (int i = 1; i < nBuckets - 1; ++i) {
                    if (cost[i] < minCost) {
                        minCost = cost[i];
                        minCostSplit = i;
                    }
                }
                // Either create leaf or split primitives at selected SAH bucket

                if (nPrimitives > (uint32_t)maxPrimsInNode || minCost < nPrimitives) {
                    const BVHPrimitiveInfo* pmid = std::partition(&buildData[start],
                        &buildData[end - 1] + 1,
                        CompareToBucket(minCostSplit, nBuckets, dim, centroidBounds));
                    mid = pmid - &buildData[0];
                } else {
                    // Create leaf BVHBuildNode
                    uint32_t firstPrimOffset = orderedPrims.size();
                    for (uint32_t i = start; i < end; ++i) {
                        uint32_t primNum = buildData[i].primitiveNumber;
                        orderedPrims.push_back(primitives[primNum]);
                    }
                    node->InitLeaf(firstPrimOffset, nPrimitives, bbox);
                }
            }
        } break;
        default:
            fprintf(stderr, "[MCUT]: error, unknown split method\n");
            break;
        }

        mid = (start + end) / 2;
        if (centroidBounds.maximum()[dim] == centroidBounds.minimum()[dim]) {
            // Create leaf BVHBuildNode
            int32_t firstPrimOffset = orderedPrims.size();
            for (uint32_t i = start; i < end; ++i) {
                uint32_t primNum = buildData[i].primitiveNumber;
                orderedPrims.push_back(primitives[primNum]);
            }
            node->InitLeaf(firstPrimOffset, nPrimitives, bbox);
            return node;
        }

        std::shared_ptr<BVHBuildNode> leftSubTree = recursiveBuild(buildData, start, mid,
            totalNodes, orderedPrims);
        std::shared_ptr<BVHBuildNode> rightSubTree = recursiveBuild(buildData, mid, end,
            totalNodes, orderedPrims);
        // Partition primitives based on splitMethod〉
        node->InitInterior(dim,
            leftSubTree,
            rightSubTree);
    }
    return node;
}

int BoundingVolumeHierarchy::GetNodeCount() const
{
    return (int)nodes.size();
}

const std::shared_ptr<LinearBVHNode>& BoundingVolumeHierarchy::GetNode(int idx) const
{
    return nodes[idx];
}

const fd_t& BoundingVolumeHierarchy::GetPrimitive(int index) const
{
    MCUT_ASSERT(index < (int)primitives.size());
    return primitives[index];
}

void BoundingVolumeHierarchy::intersectBVHTrees(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    thread_pool& scheduler,
#endif
    std::map<fd_t, std::vector<fd_t>>& symmetric_intersecting_pairs,
    const BoundingVolumeHierarchy& bvhA,
    const BoundingVolumeHierarchy& bvhB,
    const uint32_t primitiveOffsetA,
    const uint32_t primitiveOffsetB)
{
    SCOPED_TIMER(__FUNCTION__);
    MCUT_ASSERT(bvhA.GetNodeCount() > 0);
    MCUT_ASSERT(bvhB.GetNodeCount() > 0);

    auto fn_intersectBVHTrees = [&bvhA, &bvhB, &primitiveOffsetA, &primitiveOffsetB](
                                    std::vector<std::pair<int, int>>& worklist_,
                                    std::map<fd_t, std::vector<fd_t>>& symmetric_intersecting_pairs_,
                                    const uint32_t maxWorklistSize) {
        // Simultaneous DFS traversal
        while (worklist_.size() > 0 && worklist_.size() < maxWorklistSize) {
            // maxTodoSz = std::max(maxTodoSz, (int)worklist_.size());
            // std::cout << "worklist_.size()="<<worklist_.size()<<std::endl;
            std::pair<int, int> cur = worklist_.back();
            // TODO: try to keep an additional counter that allows us to minimize pushing and popping
            // Might require a wrapper class over std::vector "lazy vector"
            worklist_.pop_back();

            const uint32_t nodeAIndex = cur.first;
            const uint32_t nodeBIndex = cur.second;
            const std::shared_ptr<LinearBVHNode> nodeA = bvhA.GetNode(nodeAIndex);
            const std::shared_ptr<LinearBVHNode> nodeB = bvhB.GetNode(nodeBIndex);

            if (!intersect_bounding_boxes(nodeA->bounds, nodeB->bounds)) {
                continue;
            }

            bool nodeAIsLeaf = nodeA->nPrimitives > 0;
            bool nodeBIsLeaf = nodeB->nPrimitives > 0;

            if (nodeAIsLeaf) {
                if (nodeBIsLeaf) {
                    for (int i = 0; i < nodeA->nPrimitives; ++i) {
                        const fd_t faceA = bvhA.GetPrimitive((uint32_t)(nodeA->primitivesOffset + i));
                        const fd_t faceAOffsetted(primitiveOffsetA + faceA);

                        for (int j = 0; j < nodeB->nPrimitives; ++j) {
                            const fd_t faceB = bvhB.GetPrimitive((uint32_t)(nodeB->primitivesOffset + j));
                            const fd_t faceBOffsetted(primitiveOffsetB + faceB);

                            symmetric_intersecting_pairs_[faceAOffsetted].push_back(faceBOffsetted);
                            symmetric_intersecting_pairs_[faceBOffsetted].push_back(faceAOffsetted);
                        }
                    }
                } else {
                    const uint32_t nodeBLeftChild = nodeBIndex + 1;
                    const uint32_t nodeBRightChild = nodeB->secondChildOffset;
                    worklist_.emplace_back(nodeAIndex, nodeBLeftChild);
                    worklist_.emplace_back(nodeAIndex, nodeBRightChild);
                }
            } else {
                if (nodeBIsLeaf) {
                    const uint32_t nodeALeftChild = nodeAIndex + 1;
                    const uint32_t nodeARightChild = nodeA->secondChildOffset;
                    worklist_.emplace_back(nodeALeftChild, nodeBIndex);
                    worklist_.emplace_back(nodeARightChild, nodeBIndex);
                } else {
                    const uint32_t nodeALeftChild = nodeAIndex + 1;
                    const uint32_t nodeARightChild = nodeA->secondChildOffset;

                    const uint32_t nodeBLeftChild = nodeBIndex + 1;
                    const uint32_t nodeBRightChild = nodeB->secondChildOffset;

                    worklist_.emplace_back(nodeALeftChild, nodeBLeftChild);
                    worklist_.emplace_back(nodeALeftChild, nodeBRightChild);

                    worklist_.emplace_back(nodeARightChild, nodeBLeftChild);
                    worklist_.emplace_back(nodeARightChild, nodeBRightChild);
                }
            }
        }
    };

    // start with pair of root nodes
    std::vector<std::pair<int, int>> todo(1, std::make_pair(0, 0));

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    {
        // master thread intersects the BVHs until the number of node pairs
        // reaches a threshold (or workload was small enough that traversal
        // is finished)
        const uint32_t threshold = scheduler.get_num_threads();
        fn_intersectBVHTrees(todo, symmetric_intersecting_pairs, threshold);

        uint32_t remainingWorkloadCount = (uint32_t)todo.size(); // how much work do we still have left

        if (remainingWorkloadCount > 0) { // do parallel traversal by distributing blocks of node-pairs across worker threads
            // NOTE: we do not manage load-balancing (too complex for the perf gain)
            typedef std::vector<std::pair<int, int>>::const_iterator InputStorageIteratorType;
            typedef std::map<fd_t, std::vector<fd_t>> OutputStorageType; // symmetric_intersecting_pairs (local)

            auto fn_intersect = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageType {
                OutputStorageType symmetric_intersecting_pairs_local;

                std::vector<std::pair<int, int>> todo_local(block_start_, block_end_);

                fn_intersectBVHTrees(
                    todo_local,
                    symmetric_intersecting_pairs_local,
                    // traverse until leaves
                    std::numeric_limits<uint32_t>::max());

                return symmetric_intersecting_pairs_local;
            };

            std::vector<std::future<OutputStorageType>> futures;
            OutputStorageType partial_res;

            parallel_for(
                scheduler,
                todo.cbegin(),
                todo.cend(),
                (1 << 1),
                fn_intersect,
                partial_res, // output of master thread
                futures);

            symmetric_intersecting_pairs.insert(partial_res.cbegin(), partial_res.cend());

            for (int i = 0; i < (int)futures.size(); ++i) {
                std::future<OutputStorageType>& f = futures[i];
                MCUT_ASSERT(f.valid());
                OutputStorageType future_res = f.get();

                symmetric_intersecting_pairs.insert(future_res.cbegin(), future_res.cend());
            }
        }
    }
#else
    fn_intersectBVHTrees(todo, symmetric_intersecting_pairs, std::numeric_limits<uint32_t>::max());
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
}

#endif

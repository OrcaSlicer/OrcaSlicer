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
#ifndef MCUT_BVH_H_
#define MCUT_BVH_H_

#include "mcut/internal/hmesh.h"
#include "mcut/internal/math.h"

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
#include "mcut/internal/tpool.h"
#endif

// OIBVH is over 2-3x faster than the alternative (classic) BVH approaches.
// Our alternative BVH implementations follow: https://www.pbrt.org/chapters/pbrt-2ed-chap4.pdf
#define USE_OIBVH 1

// Expands a 10-bit integer into 30 bits by inserting 2 zeros after each bit.
extern unsigned int expandBits(unsigned int v);

// Calculates a 30-bit Morton code for the given 3D point located within the unit cube [0,1].
extern unsigned int morton3D(float x, float y, float z);

#if defined(USE_OIBVH)

// TODO: just use std::pair
typedef struct
{
    int m_left; // node-A ID (implicit index)
    int m_right; // node-B ID (implicit index)
} node_pair_t; // collision tree node

// count leading zeros in 32 bit bitfield
extern unsigned int clz(unsigned int x);

// next power of two from x
extern int next_power_of_two(int x);

// check if "x" is a power of two
extern bool is_power_of_two(int x);

// compute log-base-2 of "x"
extern int ilog2(unsigned int x);

// compute index (0...N-1) of the leaf level from the number of leaves
extern int get_leaf_level_from_real_leaf_count(const int t);

// compute tree-level index from implicit index of a node
extern int get_level_from_implicit_idx(const int bvhNodeImplicitIndex);

// compute previous power of two
extern unsigned int flp2(unsigned int x);

// compute size of of Oi-BVH give number of triangles
extern int get_ostensibly_implicit_bvh_size(const int t);

// compute left-most node on a given level
extern int get_level_leftmost_node(const int node_level);

// compute right-most leaf node in tree
extern int get_rightmost_real_leaf(const int bvhLeafLevelIndex, const int num_real_leaf_nodes_in_bvh);

// check if node is a "real node"
extern bool is_real_implicit_tree_node_id(const int bvhNodeImplicitIndex, const int num_real_leaf_nodes_in_bvh);

// get the right most real node on a given tree level
extern int get_level_rightmost_real_node(
    const int rightmostRealLeafNodeImplicitIndex,
    const int bvhLeafLevelIndex,
    const int ancestorLevelIndex);

// compute implicit index of a node's ancestor
extern int get_node_ancestor(
    const int nodeImplicitIndex,
    const int nodeLevelIndex,
    const int ancestorLevelIndex);

// calculate linear memory index of a real node
extern int get_node_mem_index(
    const int nodeImplicitIndex,
    const int leftmostImplicitIndexOnNodeLevel,
    const int bvh_data_base_offset,
    const int rightmostRealNodeImplicitIndexOnNodeLevel);

extern void build_oibvh(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    thread_pool& pool,
#endif
    const hmesh_t& mesh,
    std::vector<bounding_box_t<vec3>>& bvhAABBs,
    std::vector<fd_t>& bvhLeafNodeFaces,
    std::vector<bounding_box_t<vec3>>& face_bboxes,
    const double& slightEnlargmentEps = double(0.0));

extern void intersectOIBVHs(
    std::map<fd_t, std::vector<fd_t>>& ps_face_to_potentially_intersecting_others,
    const std::vector<bounding_box_t<vec3>>& srcMeshBvhAABBs,
    const std::vector<fd_t>& srcMeshBvhLeafNodeFaces,
    const std::vector<bounding_box_t<vec3>>& cutMeshBvhAABBs,
    const std::vector<fd_t>& cutMeshBvhLeafNodeFaces);
#else
typedef bounding_box_t<vec3> BBox;
static inline BBox Union(const BBox& a, const BBox& b)
{
    BBox out = a;
    out.expand(b);
    return out;
}

static inline BBox Union(const BBox& a, const vec3& b)
{
    BBox out = a;
    out.expand(b);
    return out;
}

enum class SplitMethod {
    SPLIT_SAH = 0,
    SPLIT_MIDDLE = 1,
    SPLIT_EQUAL_COUNTS = 2,
};

// For each primitive to be stored in the BVH, we store the centroid
// of its bounding box, its complete bounding box, and its index in
// the primitives array
struct BVHPrimitiveInfo {
    BVHPrimitiveInfo(int pn, const BBox& b)
        : primitiveNumber(pn)
        , bounds(b)
    {
        centroid = b.minimum() * (0.5) + b.maximum() * (0.5);
    }

    int primitiveNumber;
    vec3 centroid;
    bounding_box_t<vec3> bounds;
};

// Each BVHBuildNode represents a node of the BVH. All nodes store a BBox, which stores
// the bounds of all of the children beneath the node. Each interior node stores pointers to
// its two children in children. Interior nodes also record the coordinate axis along which
// primitives were sorted for distribution to their two children; this information is used to
// improve the performance of the traversal algorithm. Leaf nodes need to record which
// primitive or primitives are stored in them; the elements of the BVHAccel::primitives
// array from the offset firstPrimOffset up to but not including firstPrimOffset +
// nPrimitives are the primitives in the leaf. (Hence the need for reordering the primi-
// tives array, so that this representation can be used, rather than, for example, storing a
// variable-sized array of primitive indices at each leaf node.)
struct BVHBuildNode {
    // The BVHBuildNode constructor only initializes the children pointers; we’ll distinguish
    // between leaf and interior nodes by whether their children pointers are NULL or not,
    // respectively
    BVHBuildNode()
    {
        children[0] = children[1] = NULL;
    }

    void InitLeaf(uint32_t first, uint32_t n, const BBox& b)
    {
        firstPrimOffset = first;
        nPrimitives = n;
        bounds = b;
    }

    // The InitInterior() method requires that the two children nodes already have been cre-
    // ated, so that their pointers can be passed in. This requirement makes it easy to compute
    // the bounds of the interior node, since the children bounds are immediately available.
    void InitInterior(uint32_t axis, std::shared_ptr<BVHBuildNode>& c0, std::shared_ptr<BVHBuildNode>& c1)
    {
        children[0] = c0;
        children[1] = c1;
        bounds = Union(c0->bounds, c1->bounds);
        splitAxis = axis;
        nPrimitives = 0;
    }

    bounding_box_t<vec3> bounds;
    std::shared_ptr<BVHBuildNode> children[2];
    uint32_t splitAxis, firstPrimOffset, nPrimitives;
};

struct CompareToMid {
    CompareToMid(int d, double m)
    {
        dim = d;
        mid = m;
    }
    int dim;
    float mid;
    bool operator()(const BVHPrimitiveInfo& a) const
    {
        return a.centroid[dim] < mid;
    }
};

struct ComparePoints {
    ComparePoints(int d) { dim = d; }
    int dim;
    bool operator()(const BVHPrimitiveInfo& a,
        const BVHPrimitiveInfo& b) const
    {
        return a.centroid[dim] < b.centroid[dim];
    }
};

struct BucketInfo {
    BucketInfo() { count = 0; }
    int count;
    BBox bounds;
};

struct CompareToBucket {
    CompareToBucket(int split, int num, int d, const BBox& b)
        : centroidBounds(b)
    {
        splitBucket = split;
        nBuckets = num;
        dim = d;
    }

    // bool operator()(const BVHPrimitiveInfo &p) const;
    bool operator()(const BVHPrimitiveInfo& p) const
    {
        int b = nBuckets * ((p.centroid[dim] - centroidBounds.minimum()[dim]) / (centroidBounds.maximum()[dim] - centroidBounds.minimum()[dim]));
        if (b == nBuckets)
            b = nBuckets - 1;
        return b <= splitBucket;
    }

    int splitBucket, nBuckets, dim;
    const BBox& centroidBounds;
};

// The LinearBVHNode structure stores the information needed to traverse the BVH. In
// addition to the bounding box for each node, for leaf nodes it stores the offset and
// primitive count for the primitives in the node. For interior nodes, it stores the offset to
// the second child as well as which of the coordinate axes the primitives were partitioned
// along when the hierarchy was built; this information is used in the traversal routine below
// to try to visit nodes in front-to-back order along the ray.
struct LinearBVHNode {
    BBox bounds;
    union {
        uint32_t primitivesOffset; // leaf
        uint32_t secondChildOffset; // interior
    };
    uint8_t nPrimitives; // 0 -> interior node
    uint8_t axis; // interior node: xyz
    uint8_t pad[2]; // ensure 32 byte total size
};

class BoundingVolumeHierarchy {

public:
    BoundingVolumeHierarchy();

    ~BoundingVolumeHierarchy();

    // three stages to BVH construction
    void buildTree(const hmesh_t& mesh_,
        const fixed_precision_number_t& enlargementEps_ = fixed_precision_number_t(0.0),
        uint32_t mp_ = 1,
        const SplitMethod& sm_ = SplitMethod::SPLIT_MIDDLE);

    const BBox& GetPrimitiveBBox(int primitiveIndex) const;

    uint32_t flattenBVHTree(std::shared_ptr<BVHBuildNode> node, uint32_t* offset);

    // The initial call to recursiveBuild() is given all of the primitives to be stored in
    // the tree. It returns a pointer to the root of the tree, which is represented with
    // the BVHBuildNode structure.

    // responsible for returning a BVH for the subset of primitives represented by the range from
    // buildData[start] up to and including buildData[end-1]
    std::shared_ptr<BVHBuildNode> recursiveBuild(
        std::vector<BVHPrimitiveInfo>& buildData,
        uint32_t start,
        uint32_t end,
        uint32_t* totalNodes,
        std::vector<fd_t>& orderedPrims);

    int GetNodeCount() const;

    const std::shared_ptr<LinearBVHNode>& GetNode(int idx) const;

    const fd_t& GetPrimitive(int index) const;

    static void intersectBVHTrees(
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        thread_pool& scheduler,
#endif
        std::map<fd_t, std::vector<fd_t>>& symmetric_intersecting_pairs,
        const BoundingVolumeHierarchy& bvhA,
        const BoundingVolumeHierarchy& bvhB,
        const uint32_t primitiveOffsetA,
        const uint32_t primitiveOffsetB);

private:
    const hmesh_t* mesh;
    int maxPrimsInNode;
    SplitMethod splitMethod;
    fixed_precision_number_t enlargementEps; // used to slight enlarge BVH (with bounds of max cut-mesh perturbation magnitude)
    std::vector<BVHPrimitiveInfo> buildData;
    std::vector<fd_t> primitives; // ordered primitives
    std::vector<BBox> primitiveOrderedBBoxes; // unsorted elements correspond to mesh indices
    std::vector<std::shared_ptr<LinearBVHNode>> nodes;
};
#endif // #if defined(USE_OIBVH)

#endif // MCUT_BVH_H_

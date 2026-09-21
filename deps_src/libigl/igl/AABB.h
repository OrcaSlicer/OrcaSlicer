// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_AABB_H
#define IGL_AABB_H

#include "Hit.h"
#include "igl_inline.h"
#include <cassert>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <vector>
namespace igl
{
  /// Implementation of semi-general purpose axis-aligned bounding box hierarchy.
  /// The mesh (V,Ele) is stored and managed by the caller and each routine here
  /// simply takes it as references (it better not change between calls).
  ///
  /// It's a little annoying that the Dimension is a template parameter and not
  /// picked up at run time from V. This leads to duplicated code for 2d/3d (up to
  /// dim).
  ///
  /// @tparam DerivedV  Matrix type of vertex positions (e.g., `Eigen::MatrixXd`)
  /// @tparam DIM Dimension of mesh vertex positions (2 or 3)
  template <typename DerivedV, int DIM>
    class AABB
    {
public:
///////////////////////////////////////////////////////////////////////////////
// Member variables
///////////////////////////////////////////////////////////////////////////////
      /// Scalar type of vertex positions (e.g., `double`)
      typedef typename DerivedV::Scalar Scalar;
      /// Fixed-size (`DIM`) RowVector type using `Scalar`
      typedef Eigen::Matrix<Scalar,1,DIM> RowVectorDIMS;
      /// Fixed-size (`DIM`) (Column)Vector type using `Scalar`
      typedef Eigen::Matrix<Scalar,DIM,1> VectorDIMS;
      /// Fixed-width (`DIM`) Matrix type using `Scalar`
      typedef Eigen::Matrix<Scalar,Eigen::Dynamic,DIM> MatrixXDIMS;
      /// Pointer to "left" child node (`nullptr` if leaf)
      // Shared pointers are slower...
      AABB * m_left;
      /// Pointer to "right" child node (`nullptr` if leaf)
      AABB * m_right;
      /// Pointer to "parent" node (`nullptr` if root)
      AABB * m_parent;
      /// Axis-Aligned Bounding Box containing this node
      Eigen::AlignedBox<Scalar,DIM> m_box;
      /// Index of single primitive in this node if full leaf, otherwise -1 for non-leaf
      int m_primitive;
///////////////////////////////////////////////////////////////////////////////
// Non-templated member functions
///////////////////////////////////////////////////////////////////////////////
      //Scalar m_low_sqr_d;
      //int m_depth;
      /// @private
      AABB():
        m_left(nullptr), m_right(nullptr),m_parent(nullptr),
        m_box(), m_primitive(-1)
        //m_low_sqr_d(std::numeric_limits<double>::infinity()),
        //m_depth(0)
      {
        static_assert(DerivedV::ColsAtCompileTime == DIM || DerivedV::ColsAtCompileTime == Eigen::Dynamic,"DerivedV::ColsAtCompileTime == DIM || DerivedV::ColsAtCompileTime == Eigen::Dynamic");
      }
      /// @private
      // http://stackoverflow.com/a/3279550/148668
      AABB(const AABB& other):
        m_left (other.m_left  ? new AABB(*other.m_left)  : nullptr),
        m_right(other.m_right ? new AABB(*other.m_right) : nullptr),
        m_parent(other.m_parent),
        m_box(other.m_box),
        m_primitive(other.m_primitive)
        //m_low_sqr_d(other.m_low_sqr_d),
        //m_depth(std::max(
        //   m_left ? m_left->m_depth + 1 : 0,
        //   m_right ? m_right->m_depth + 1 : 0))
        {
          if(m_left) { m_left->m_parent = this; }
          if(m_right) { m_right->m_parent = this; }
        }
      /// @private
      // copy-swap idiom
      friend void swap(AABB& first, AABB& second)
      {
        // Enable ADL
        using std::swap;
        swap(first.m_left,second.m_left);
        swap(first.m_right,second.m_right);
        swap(first.m_parent,second.m_parent);
        swap(first.m_box,second.m_box);
        swap(first.m_primitive,second.m_primitive);
        //swap(first.m_low_sqr_d,second.m_low_sqr_d);
        //swap(first.m_depth,second.m_depth);
      }
      /// @private
      // Pass-by-value (aka copy)
      AABB& operator=(AABB other)
      {
        swap(*this,other);
        return *this;
      }
      /// @private
      AABB(AABB&& other):
        // initialize via default constructor
        AABB()
      {
        swap(*this,other);
      }
      /// @private
      // Seems like there should have been an elegant solution to this using
      // the copy-swap idiom above:
      IGL_INLINE void clear()
      {
        m_primitive = -1;
        m_box = Eigen::AlignedBox<Scalar,DIM>();
        delete m_left;
        m_left = nullptr;
        delete m_right;
        m_right = nullptr;
        // Tell my parent I'm dead
        if(m_parent)
        {
          if(m_parent->m_left == this)
          {
            m_parent->m_left = nullptr;
          }else if(m_parent->m_right == this)
          {
            m_parent->m_right = nullptr;
          }else
          {
            assert(false && "I'm not my parent's child");
          }
          auto * grandparent = m_parent->m_parent;
          if(grandparent)
          {
            // Before
            //     grandparent
            //        ╱     ╲
            //    parent   pibling
            //      ╱    ╲
            // sibling  this
            //
            // After
            //     grandparent
            //        ╱     ╲
            //    sibling®  pibling
          }else
          {
            // Before
            //    parent=root
            //      ╱    ╲
            // sibling  this
            //
            // After
            //     grandparent
            //        ╱     ╲
            //    sibling®  pibling
          }
        }
        // Now my parent is dead to me.
        m_parent = nullptr;
      }
      /// @private
      ~AABB()
      {
        clear();
      }
      /// Return whether at leaf node
      IGL_INLINE bool is_leaf() const;
      /// Return whether at root node
      IGL_INLINE bool is_root() const;
      /// Return the root node of this node's tree by following its parent
      IGL_INLINE AABB<DerivedV,DIM>* root() const;
      IGL_INLINE AABB<DerivedV,DIM>* detach();
      IGL_INLINE void refit_lineage();
      /// Get a vector of leaves indexed by their m_primitive id (these better
      /// be non-negative and tightly packed.
      /// @param[in] m  number of leaves/elements (Ele.rows())
      /// @returns leaves  m list of pointers to leaves
      IGL_INLINE std::vector<AABB<DerivedV,DIM>*> gather_leaves(const int m);
      /// \overload where m is the max m_primitive id in the tree.
      IGL_INLINE std::vector<AABB<DerivedV,DIM>*> gather_leaves();
      /// Pad leaves by `pad` in each dimension
      /// @param[in] pad padding amount
      /// @param[in] polish_rotate_passes number of passes to polish rotations
      /// @returns pointer to (potentially new) root
      IGL_INLINE AABB<DerivedV,DIM>* pad(
        const std::vector<AABB<DerivedV,DIM>*> & leaves,
        const Scalar pad,
        const int polish_rotate_passes=0);
      /// @returns `this` if no update was needed, otherwise returns pointer to
      /// (potentially new) root
      ///
      /// Example:
      /// ```cpp
      /// auto * up = leaf->update(new_box);
      /// if(up != leaf)
      /// {
      ///   tree = up->root();
      /// }else
      /// {
      ///   printf("no update occurred\n");
      /// }
      ///
      /// // or simply
      /// tree = leaf->update(new_box)->root();
      /// ```
      IGL_INLINE AABB<DerivedV,DIM>* update(
          const Eigen::AlignedBox<Scalar,DIM> & new_box,
          const Scalar pad=0);
      /// Insert a (probably a leaf) AABB `other` into this AABB tree. If
      /// `other`'s box is contained in this AABB's box then insert it as a child recursively.
      ///
      /// If `other`'s box is not contained in this AABB's box then insert it as a
      /// sibling.
      ///
      /// It's a very good idea to call either `rotate` (faster, less good) or `rotate_lineage` (slower, better)
      /// after insertion. Rotating continues to improve the tree's quality so
      /// after doing a bunch of insertions you might even consider calling
      /// `rotate` on all nodes.
      ///
      /// `insert` attempts to minimize total internal surface area. Where as
      /// `init` is top-down and splits boxes based on the median along the
      /// longest dimension. When initializing a tree, `init` seems to result in
      /// great trees (small height and small total internal surface area).
      ///
      /// @param[in] other pointer to another AABB node
      /// @returns pointer to the parent of `other` or `other` itself. This
      /// could be == to a `new`ly created internal node or to `other` if
      /// `this==other`. Calling ->root() on this returned node will give you
      /// the root of the tree.
      ///
      /// ##### Example
      ///
      /// ```cpp
      /// // Create a tree (use pointer to track changes to root)
      /// auto * tree = new igl::AABB<DerivedV,3>::AABB();
      /// // Fill the tree (e.g., using ->init())
      /// …
      /// // Create a new leafe node
      /// auto * leaf = new igl::AABB<DerivedV,3>::AABB();
      /// // Fill the leaf node with a primitive and box
      /// …
      /// // Insert into the tree and find the possibly new root
      /// tree = tree->insert(leaf)->root();
      /// ```
      IGL_INLINE AABB<DerivedV,DIM>* insert(AABB * other);
      /// Insert `other` as a sibling to `this` by creating a new internal node
      /// to be their shared parent.
      ///
      ///     Before
      ///              parent
      ///              ╱     ╲
      ///          this(C)  sibling
      ///            ╱   ╲
      ///          left right
      ///
      ///     After
      ///              parent
      ///              ╱     ╲
      ///           newbie   sibling
      ///            ╱    ╲
      ///         this    other
      ///         ╱     ╲
      ///       left  right
      ///
      ///
      /// @param[in] other pointer to another AABB node
      /// @returns pointer to the new shared parent.
      IGL_INLINE AABB<DerivedV,DIM>* insert_as_sibling(AABB * other);
      /// Try to swap this node with its close relatives if it will decrease
      /// total internal surface area.
      ///
      ///
      ///            grandparent
      ///            ╱          ╲
      ///         parent       pibling°
      ///         ╱     ╲          ╱     ╲
      ///     sibling  this   cuz1°  cuz2°
      ///      ╱     ╲
      ///     nib1° nib2°
      ///
      ///     °Swap Candidates
      ///
      /// @param[in] dry_run  if true then don't actually swap
      /// @return[in] the change in total internal surface area, 0 if no
      /// improvement and rotate won't be carried out.
      IGL_INLINE Scalar rotate(const bool dry_run = false);
      /// Try to swap this node with its cousins if it will decrease
      /// total internal surface area.
      ///
      /// @param[in] dry_run  if true then don't actually swap
      /// @return[in] the change in total internal surface area, 0 if no
      /// improvement and rotate won't be carried out.
      ///
      ///     Before
      ///            grandparent
      ///            ╱          ╲
      ///         parent       pibling
      ///         ╱     ╲          ╱     ╲
      ///     sibling  this    cuz1  cuz2
      ///
      ///
      ///     Candidates
      ///            grandparent
      ///            ╱          ╲
      ///         parent       pibling
      ///         ╱     ╲          ╱     ╲
      ///     sibling  cuz1   this   cuz2
      ///
      ///     Or
      ///            grandparent
      ///            ╱          ╲
      ///         parent       pibling
      ///         ╱     ╲          ╱     ╲
      ///     sibling  cuz2    cuz1  this
      IGL_INLINE Scalar rotate_across(const bool dry_run = false);
      /// Try to swap this node with its pibling if it will decrease
      /// total internal surface area.
      ///
      /// @param[in] dry_run  if true then don't actually swap
      /// @return[in] the change in total internal surface area, 0 if no
      /// improvement and rotate won't be carried out.
      ///
      ///     Before
      ///        grandparent
      ///           ╱     ╲
      ///         other  parent
      ///                ╱   ╲
      ///             this  sibling
      ///
      ///
      ///     Candidate
      ///        grandparent
      ///           ╱     ╲
      ///        this    parent
      ///                ╱   ╲
      ///            other  sibling
      IGL_INLINE Scalar rotate_up(const bool dry_run = false);
      /// Try to swap this node with one of its niblings if it will decrease
      /// total internal surface area.
      ///
      /// @param[in] dry_run  if true then don't actually swap
      /// @return[in] the change in total internal surface area, 0 if no
      /// improvement and rotate won't be carried out.
      ///
      ///     Before
      ///           parent
      ///           ╱     ╲
      ///         this   sibling
      ///                ╱   ╲
      ///             left  right
      ///
      ///
      ///     Candidates
      ///           parent
      ///           ╱     ╲
      ///       left     sibling
      ///                ╱   ╲
      ///            this   right
      ///
      ///     Or
      ///
      ///           parent
      ///           ╱     ╲
      ///       right    sibling
      ///                ╱   ╲
      ///            left   this
      IGL_INLINE Scalar rotate_down(const bool dry_run = false);
      /// "Rotate" (swap) `reining` with `challenger`.
      ///
      ///     Before
      ///        grandparent
      ///           ╱       ╲
      ///      reining      parent
      ///                   ╱     ╲
      ///            challenger  sibling
      ///
      ///
      ///     Candidate
      ///        grandparent
      ///           ╱       ╲
      ///     challenger    parent
      ///                   ╱     ╲
      ///              reining   sibling
      /// @param[in] reining  pointer to AABB node to be rotated
      /// @param[in] grandparent  pointer to challenger's grandparent
      /// @param[in] parent  pointer to challenger's parent
      /// @param[in] challenger  pointer to AABB node to be rotated
      /// @param[in] sibling  pointer to challenger's sibling
      /// @returns true only if rotation was possible and successfully carried
      /// out.
      static IGL_INLINE Scalar rotate_up(
        const bool dry_run,
        AABB<DerivedV,DIM>* reining,
        AABB<DerivedV,DIM>* grandparent,
        AABB<DerivedV,DIM>* parent,
        AABB<DerivedV,DIM>* challenger,
        AABB<DerivedV,DIM>* sibling);
      // Should this be a static function with an argument?
      IGL_INLINE void rotate_lineage();
      /// Number of nodes contained in subtree (is it?)
      ///
      /// \note At best, this function has a dubious name. This is really an
      /// internal helper function for the serialization.
      ///
      /// \see size()
      ///
      /// @return Number of elements m then total tree size should be 2*h where h is
      /// the deepest depth 2^ceil(log(#Ele*2-1))
      IGL_INLINE int subtree_size() const;
      /// @param[in]  box  query box
      /// @param[in,out] leaves  list of leaves to append to
      IGL_INLINE bool append_intersecting_leaves(
        const Eigen::AlignedBox<Scalar,DIM> & box,
        std::vector<const AABB<DerivedV,DIM>*> & leaves) const;
      /// Compute sum of surface area of all internal (non-root, non-leaf) boxes
      IGL_INLINE typename DerivedV::Scalar internal_surface_area() const;
      /// Validate the subtree under this node by running a bunch of assertions.
      /// Does nothing when not in debug mode
      IGL_INLINE void validate() const;
      /// print the memory addresses of the tree in a somewhat legible way
      IGL_INLINE void print(const int depth = 0) const;
      /// @returns Actual size of tree. Total number of nodes in tree. A singleton root
      /// has size 1.
      ///
      /// \see subtree_size
      IGL_INLINE int size() const;
      /// @returns Height of the tree. A singleton root has height 1.
      IGL_INLINE int height() const;
private:
      /// If new distance (sqr_d_candidate) is less than current distance
      /// (sqr_d), then update this distance and its associated values
      /// _in-place_:
      ///
      /// @param[in] p  dim-long query point (only used in DEBUG mode)
      /// @param[in] sqr_d  candidate minimum distance for this query, see
      ///   output
      /// @param[in] i  candidate index into Ele of closest point, see output
      /// @param[in] c  dim-long candidate closest point, see output
      /// @param[in] sqr_d  current minimum distance for this query, see output
      /// @param[in] i  current index into Ele of closest point, see output
      /// @param[in] c  dim-long current closest point, see output
      /// @param[out] sqr_d   minimum of initial value and squared distance to
      ///   this primitive
      /// @param[out] i  possibly updated index into Ele of closest point
      /// @param[out] c  dim-long possibly updated closest point
      IGL_INLINE void set_min(
        const RowVectorDIMS & p,
        const Scalar sqr_d_candidate,
        const int & i_candidate,
        const RowVectorDIMS & c_candidate,
        Scalar & sqr_d,
        int & i,
        Eigen::PlainObjectBase<RowVectorDIMS> & c) const;
public:
///////////////////////////////////////////////////////////////////////////////
// Templated member functions
///////////////////////////////////////////////////////////////////////////////
      /// Build an Axis-Aligned Bounding Box tree for a given mesh and given
      /// serialization of a previous AABB tree.
      ///
      /// @param[in] V  #V by dim list of mesh vertex positions.
      /// @param[in] Ele  #Ele by dim+1 list of mesh indices into #V.
      /// @param[in] bb_mins  max_tree by dim list of bounding box min corner
      ///   positions
      /// @param[in] bb_maxs  max_tree by dim list of bounding box max corner
      ///   positions
      /// @param[in] elements  max_tree list of element or (not leaf id) indices
      ///   into Ele
      /// @param[in] i  recursive call index {0}
      template <
        typename DerivedEle,
        typename Derivedbb_mins,
        typename Derivedbb_maxs,
        typename Derivedelements>
        IGL_INLINE void init(
            const Eigen::MatrixBase<DerivedV> & V,
            const Eigen::MatrixBase<DerivedEle> & Ele,
            const Eigen::MatrixBase<Derivedbb_mins> & bb_mins,
            const Eigen::MatrixBase<Derivedbb_maxs> & bb_maxs,
            const Eigen::MatrixBase<Derivedelements> & elements,
            const int i = 0);
      /// Build an Axis-Aligned Bounding Box tree for a given mesh and given
      /// serialization of a previous AABB tree.
      ///
      /// @param[in] V  #V by dim list of mesh vertex positions.
      /// @param[in] Ele  #Ele by dim+1 list of mesh indices into #V.
      template <typename DerivedEle>
      IGL_INLINE void init(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedEle> & Ele);
      /// Build an Axis-Aligned Bounding Box tree for a given mesh.
      ///
      /// @param[in] V  #V by dim list of mesh vertex positions.
      /// @param[in] Ele  #Ele by dim+1 list of mesh indices into #V.
      /// @param[in] SI  #Ele by dim list revealing for each coordinate where
      ///   Ele's barycenters would be sorted: SI(e,d) = i --> the dth
      ///   coordinate of the barycenter of the eth element would be placed at
      ///   position i in a sorted list.
      /// @param[in] I  #I list of indices into Ele of elements to include (for
      ///   recursive calls)
      ///
      template <typename DerivedEle, typename DerivedSI, typename DerivedI>
      IGL_INLINE void init(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedEle> & Ele,
          const Eigen::MatrixBase<DerivedSI> & SI,
          const Eigen::MatrixBase<DerivedI>& I);
      /// @returns `this` if no update was needed, otherwise returns pointer to
      /// (potentially new) root
      template <typename DerivedEle>
      IGL_INLINE AABB<DerivedV,DIM>* update_primitive(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedEle> & Ele,
          const Scalar pad=0);

      /// Find the indices of elements containing given point: this makes sense
      /// when Ele is a co-dimension 0 simplex (tets in 3D, triangles in 2D).
      ///
      /// @param[in]  V  #V by dim list of mesh vertex positions. **Should be
      ///   same as used to construct mesh.**
      /// @param[in]  Ele  #Ele by dim+1 list of mesh indices into #V. **Should
      ///   be same as used to construct mesh.**
      /// @param[in]  q  dim row-vector query position
      /// @param[in]  first  whether to only return first element containing q
      /// @return  list of indices of elements containing q
      template <typename DerivedEle, typename Derivedq>
      IGL_INLINE std::vector<int> find(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedEle> & Ele,
          const Eigen::MatrixBase<Derivedq> & q,
          const bool first=false) const;

      /// Serialize this class into 3 arrays (so we can pass it pack to matlab)
      ///
      /// @param[out]  bb_mins  max_tree by dim list of bounding box min corner
      ///   positions
      /// @param[out]  bb_maxs  max_tree by dim list of bounding box max corner
      ///   positions
      /// @param[out]  elements  max_tree list of element or (not leaf id)
      ///   indices into Ele
      /// @param[in]  i  recursive call index into these arrays {0}
      template <
        typename Derivedbb_mins,
        typename Derivedbb_maxs,
        typename Derivedelements>
        IGL_INLINE void serialize(
            Eigen::PlainObjectBase<Derivedbb_mins> & bb_mins,
            Eigen::PlainObjectBase<Derivedbb_maxs> & bb_maxs,
            Eigen::PlainObjectBase<Derivedelements> & elements,
            const int i = 0) const;
      /// Compute squared distance to a query point
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  p  dim-long query point
      /// @param[out]  i  facet index corresponding to smallest distances
      /// @param[out]  c  closest point
      /// @return squared distance
      ///
      /// \pre Currently assumes Elements are triangles regardless of
      /// dimension.
      template <typename DerivedEle>
      IGL_INLINE Scalar squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & p,
        int & i,
        Eigen::PlainObjectBase<RowVectorDIMS> & c) const;
      /// Compute squared distance to a query point if within `low_sqr_d` and
      /// `up_sqr_d`.
      ///
      /// @param[in] V  #V by dim list of vertex positions
      /// @param[in] Ele  #Ele by dim list of simplex indices
      /// @param[in] p  dim-long query point
      /// @param[in] low_sqr_d  lower bound on squared distance, specified
      ///   maximum squared distance
      /// @param[in] up_sqr_d  current upper bounded on squared distance,
      ///   current minimum squared distance (only consider distances less than
      ///   this), see output.
      /// @param[out]  i  facet index corresponding to smallest distances
      /// @param[out]  c  closest point
      /// @return squared distance
      ///
      /// \pre currently assumes Elements are triangles regardless of
      /// dimension.
      template <typename DerivedEle>
      IGL_INLINE Scalar squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & p,
        const Scalar low_sqr_d,
        const Scalar up_sqr_d,
        int & i,
        Eigen::PlainObjectBase<RowVectorDIMS> & c) const;
      /// Compute squared distance to a query point (default `low_sqr_d`)
      ///
      /// @param[in] V  #V by dim list of vertex positions
      /// @param[in] Ele  #Ele by dim list of simplex indices
      /// @param[in] p  dim-long query point
      /// @param[in] up_sqr_d  current upper bounded on squared distance,
      ///   current minimum squared distance (only consider distances less than
      ///   this), see output.
      /// @param[out]  i  facet index corresponding to smallest distances
      /// @param[out]  c  closest point
      /// @return squared distance
      ///
      template <typename DerivedEle>
      IGL_INLINE Scalar squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & p,
        const Scalar up_sqr_d,
        int & i,
        Eigen::PlainObjectBase<RowVectorDIMS> & c) const;
      /// Intersect a ray with the mesh return all hits
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  origin  dim-long ray origin
      /// @param[in]  dir  dim-long ray direction
      /// @param[out]  hits  list of hits
      /// @return  true if any hits
      template <typename DerivedEle>
      IGL_INLINE bool intersect_ray(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & origin,
        const RowVectorDIMS & dir,
        std::vector<igl::Hit<typename DerivedV::Scalar>> & hits) const;
      /// Intersect a ray with the mesh return first hit
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  origin  dim-long ray origin
      /// @param[in]  dir  dim-long ray direction
      /// @param[out]  hit  first hit
      /// @return  true if any hit
      template <typename DerivedEle>
      IGL_INLINE bool intersect_ray(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & origin,
        const RowVectorDIMS & dir,
        igl::Hit<typename DerivedV::Scalar> & hit) const;

      /// Intersect a ray with the mesh return first hit farther than `min_t`
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  origin  dim-long ray origin
      /// @param[in]  dir  dim-long ray direction
      /// @param[in]  min_t  minimum t value to consider
      /// @param[out]  hit  first hit
      /// @return  true if any hit
      template <typename DerivedEle>
      IGL_INLINE bool intersect_ray(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & origin,
        const RowVectorDIMS & dir,
        const Scalar min_t,
        igl::Hit<typename DerivedV::Scalar> & hit) const;
      /// Intersect a rays with the mesh return first hit for each
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  origin #ray by dim+1 list of ray origins
      /// @param[in]  dir #ray by dim list of ray directions
      /// @param[in]  min_t  minimum t value to consider
      /// @param[out]  I #ray list of indices into Ele of closest primitives
      ///   (-1 indicates no hit)
      /// @param[out]  T #ray list of t values (nan indicates no hit)
      /// @param[out]  UV #ray by dim list of barycentric coordinates
      template <
        typename DerivedEle,
        typename DerivedOrigin,
        typename DerivedDir,
        typename DerivedI,
        typename DerivedT,
        typename DerivedUV>
      IGL_INLINE void intersect_ray(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const Eigen::MatrixBase<DerivedOrigin> & origin,
        const Eigen::MatrixBase<DerivedDir> & dir,
        const Scalar min_t,
        Eigen::PlainObjectBase<DerivedI> & I,
        Eigen::PlainObjectBase<DerivedT> & T,
        Eigen::PlainObjectBase<DerivedUV> & UV);
      template <
        typename DerivedEle,
        typename DerivedOrigin,
        typename DerivedDir>
      IGL_INLINE void intersect_ray(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const Eigen::MatrixBase<DerivedOrigin> & origin,
        const Eigen::MatrixBase<DerivedDir> & dir,
        std::vector<std::vector<igl::Hit<typename DerivedV::Scalar>>> & hits);
      /// Compute the squared distance from all query points in P to the
      /// _closest_ points on the primitives stored in the AABB hierarchy for
      /// the mesh (V,Ele).
      ///
      /// @param[in] V  #V by dim list of vertex positions
      /// @param[in] Ele  #Ele by dim list of simplex indices
      /// @param[in] P  #P by dim list of query points
      /// @param[out] sqrD  #P list of squared distances
      /// @param[out] I  #P list of indices into Ele of closest primitives
      /// @param[out] C  #P by dim list of closest points
      template <
        typename DerivedEle,
        typename DerivedP,
        typename DerivedsqrD,
        typename DerivedI,
        typename DerivedC>
      IGL_INLINE void squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const Eigen::MatrixBase<DerivedP> & P,
        Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
        Eigen::PlainObjectBase<DerivedI> & I,
        Eigen::PlainObjectBase<DerivedC> & C) const;
      /// Compute the squared distance from all query points in P already stored
      /// in its own AABB hierarchy to the _closest_ points on the primitives
      /// stored in the AABB hierarchy for the mesh (V,Ele).
      ///
      /// @param[in] V  #V by dim list of vertex positions
      /// @param[in] Ele  #Ele by dim list of simplex indices
      /// @param[in] other  AABB hierarchy of another set of primitives (must be points)
      /// @param[in] other_V  #other_V by dim list of query points
      /// @param[in] other_Ele  #other_Ele by ss list of simplex indices into other_V
      ///     (must be simple list of points: ss == 1)
      /// @param[out] sqrD  #P list of squared distances
      /// @param[out] I  #P list of indices into Ele of closest primitives
      /// @param[out] C  #P by dim list of closest points
      template <
        typename DerivedEle,
        typename Derivedother_V,
        typename Derivedother_Ele,
        typename DerivedsqrD,
        typename DerivedI,
        typename DerivedC>
      IGL_INLINE void squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const AABB<Derivedother_V,DIM> & other,
        const Eigen::MatrixBase<Derivedother_V> & other_V,
        const Eigen::MatrixBase<Derivedother_Ele> & other_Ele,
        Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
        Eigen::PlainObjectBase<DerivedI> & I,
        Eigen::PlainObjectBase<DerivedC> & C) const;
private:
      template <
        typename DerivedEle,
        typename Derivedother_V,
        typename Derivedother_Ele,
        typename DerivedsqrD,
        typename DerivedI,
        typename DerivedC>
      IGL_INLINE Scalar squared_distance_helper(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const AABB<Derivedother_V,DIM> * other,
        const Eigen::MatrixBase<Derivedother_V> & other_V,
        const Eigen::MatrixBase<Derivedother_Ele>& other_Ele,
        const Scalar up_sqr_d,
        Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
        Eigen::PlainObjectBase<DerivedI> & I,
        Eigen::PlainObjectBase<DerivedC> & C) const;
      // Compute the squared distance to the primitive in this node: assumes
      // that this is indeed a leaf node.
      //
      // Inputs:
      //   V  #V by dim list of vertex positions
      //   Ele  #Ele by dim list of simplex indices
      //   p  dim-long query point
      //   sqr_d  current minimum distance for this query, see output
      //   i  current index into Ele of closest point, see output
      //   c  dim-long current closest point, see output
      // Outputs:
      //   sqr_d   minimum of initial value and squared distance to this
      //     primitive
      //   i  possibly updated index into Ele of closest point
      //   c  dim-long possibly updated closest point
      template <typename DerivedEle>
      IGL_INLINE void leaf_squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & p,
        const Scalar low_sqr_d,
        Scalar & sqr_d,
        int & i,
        Eigen::PlainObjectBase<RowVectorDIMS> & c) const;
      // Default low_sqr_d
      template <typename DerivedEle>
      IGL_INLINE void leaf_squared_distance(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & p,
        Scalar & sqr_d,
        int & i,
        Eigen::PlainObjectBase<RowVectorDIMS> & c) const;
      /// Intersect a ray with the mesh return all hits
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  origin  dim-long ray origin
      /// @param[in]  dir  dim-long ray direction
      /// @param[out]  hits  list of hits
      /// @return  true if any hits
      template <typename DerivedEle>
      IGL_INLINE bool intersect_ray_opt(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & origin,
        const RowVectorDIMS & dir,
        const RowVectorDIMS & inv_dir,
        const RowVectorDIMS & inv_dir_pad,
        std::vector<igl::Hit<typename DerivedV::Scalar>> & hits) const;
      /// Intersect a ray with the mesh return first hit farther than `min_t`
      ///
      /// @param[in]  V  #V by dim list of vertex positions
      /// @param[in]  Ele  #Ele by dim list of simplex indices
      /// @param[in]  origin  dim-long ray origin
      /// @param[in]  dir  dim-long ray direction
      /// @param[in]  min_t  minimum t value to consider
      /// @param[out]  hit  first hit
      /// @return  true if any hit
      template <typename DerivedEle>
      IGL_INLINE bool intersect_ray_opt(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedEle> & Ele,
        const RowVectorDIMS & origin,
        const RowVectorDIMS & dir,
        const RowVectorDIMS & inv_dir,
        const RowVectorDIMS & inv_dir_pad,
        const Scalar min_t,
        igl::Hit<typename DerivedV::Scalar> & hit) const;
public:
      EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    };
}


#ifndef IGL_STATIC_LIBRARY
#  include "AABB.cpp"
#endif

#endif

#ifndef IGL_INTERSECTION_BLOCKING_COLLAPSE_EDGE_CALLBACKS_H
#define IGL_INTERSECTION_BLOCKING_COLLAPSE_EDGE_CALLBACKS_H
#include "igl_inline.h"
#include "decimate_callback_types.h"
#include "placeholders.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  // Forward declaration
  template <typename DerivedV, int DIM> class AABB;
  /// Wrap around callbacks for decimate to prevent intersections from being
  /// created.
  ///
  /// @param[in] orig_pre_collapse  original pre_collapse callback (callers copy
  ///   can be destroyed)
  /// @param[in] orig_post_collapse original post_collapse callback (ditto)
  /// @param[in] leaves  list of pointers to leaves of tree (ditto)
  /// @param[in,out] tree  pointer to AABB tree whose leaves will correspond to
  ///   the current (non-null) faces in (V,F)  (callers copy can _not_ be
  ///   destroyed – not the tree and not this pointer – until use of
  ///   pre_/post_collapse is done)
  /// @param[out] pre_collapse  new pre_collapse callback
  /// @param[out] post_collapse  new post_collapse callback
  ///
  /// #### Example
  ///
  /// ```cpp
  ///  // Build tree around mesh (must be edge-manifold, may have boundaries)
  ///  igl::AABB<Eigen::MatrixXd, 3> * tree = new igl::AABB<Eigen::MatrixXd, 3>();
  ///  tree->init(V,F);
  ///  // Connect boundaries to dummy infinite vertex (after tree building)
  ///  Eigen::MatrixXd VO;
  ///  Eigen::MatrixXi FO;
  ///  igl::connect_boundary_to_infinity(V,F,VO,FO);
  ///  // prepare edge structures
  ///  Eigen::VectorXi EMAP;
  ///  Eigen::MatrixXi E,EF,EI;
  ///  igl::edge_flaps(FO,E,EMAP,EF,EI);
  ///  // prepare callbacks
  ///  igl::decimate_cost_and_placement_callback cost_and_placement;
  ///  igl::decimate_pre_collapse_callback  pre_collapse;
  ///  igl::decimate_post_collapse_callback post_collapse;
  ///  cost_and_placement = igl::shortest_edge_and_midpoint;
  ///  igl::decimate_trivial_callbacks(pre_collapse,post_collapse);
  ///  igl::intersection_blocking_collapse_edge_callbacks(
  ///      pre_collapse, post_collapse, // These will get copied as needed
  ///      tree,
  ///      pre_collapse, post_collapse);
  ///  int m = F.rows();
  ///  const int orig_m = m;
  ///  Eigen::MatrixXd U;
  ///  Eigen::MatrixXi G;
  ///  Eigen::VectorXi J,I;
  ///  const bool ret = igl::decimate(
  ///    VO, FO,
  ///    cost_and_placement,
  ///    igl::max_faces_stopping_condition(m,orig_m,target_m),
  ///    pre_collapse,
  ///    post_collapse,
  ///    E, EMAP, EF, EI,
  ///    U, G, J, I);
  ///  G = G(igl::find((J.array()<orig_m).eval()), igl::placeholders::all).eval();
  ///  {
  ///    Eigen::VectorXi _;
  ///    igl::remove_unreferenced(Eigen::MatrixXd(U),Eigen::MatrixXi(G),U,G,_);
  ///  }
  /// ```
  ///
  /// \see decimate.h
  IGL_INLINE void intersection_blocking_collapse_edge_callbacks(
    const decimate_pre_collapse_callback  & orig_pre_collapse,
    const decimate_post_collapse_callback & orig_post_collapse,
    const std::vector<AABB<Eigen::MatrixXd,3> *> & leaves,
          AABB<Eigen::MatrixXd,3> * & tree,
          decimate_pre_collapse_callback  & pre_collapse,
          decimate_post_collapse_callback & post_collapse);
  /// \overload Same as above but computes leaves from tree
  IGL_INLINE void intersection_blocking_collapse_edge_callbacks(
    const decimate_pre_collapse_callback  & orig_pre_collapse,
    const decimate_post_collapse_callback & orig_post_collapse,
          AABB<Eigen::MatrixXd,3> * & tree,
          decimate_pre_collapse_callback  & pre_collapse,
          decimate_post_collapse_callback & post_collapse);
  /// \overload Same as above but uses trivial callbacks
  IGL_INLINE void intersection_blocking_collapse_edge_callbacks(
    AABB<Eigen::MatrixXd,3> * & tree,
    decimate_pre_collapse_callback  & pre_collapse,
    decimate_post_collapse_callback & post_collapse);
}

#ifndef IGL_STATIC_LIBRARY
#  include "intersection_blocking_collapse_edge_callbacks.cpp"
#endif
#endif

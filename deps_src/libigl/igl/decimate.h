// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DECIMATE_H
#define IGL_DECIMATE_H
#include "igl_inline.h"
#include "decimate_callback_types.h"
#include "COLLAPSE_EDGE_NULL.h"
#include <Eigen/Core>

/// @file decimate.h 
///
/// igl::decimate implements a customizable greedy edge collapser using a priority-queue:
///
///      Q ← {}
///      for each edge e
///        cost[e], placement[e] ← cost_and_placement(e)
///        Q.update( e, cost[e] )
///      
///      while Q not empty
///         e ← Q.pop()
///         if cost[e] is finite 
///            collapse_happened_flag = false
///            if pre_collapse( e ) 
///              collapse_happened_flag = collapse_edge( e )
///            post_collapse( e, collapse_happened_flag )
///            if collapse_happened_flag
///              for each neighbor edge f
///                cost[f], placement[f] ← cost_and_placement(f)
///                Q.update( f, cost[f] )
///              if stopping_condition()
///                break
///            else
///               cost[ e ] = ∞
///                Q.update( e, cost[e] )
///          
///
/// There are four important callbacks that can be customized (decimate_callback_types.h). Note that any of these functions can capture user-defined book-keeping variables.
/// 
/// `cost_and_placement` This function is called for every original edge before
/// the queue processing starts and then on the “neighbors” (edges in the
/// two-ring) of a successfully collapsed edge. The function should output a
/// cost that would be paid if the given edge were to be collapsed to a single
/// vertex and the positional placement of that new vertex. Outputting a cost of
/// ∞ guarantees that the edge will not be collapsed.
/// 
/// `pre_collapse` This function is called just before `collapse_edge` is
/// attempted. If this function returns false then the collapse is aborted. This
/// callback is an opportunity for callers to conduct a final check whether the
/// collapse should really take place (e.g., based on the most current
/// information, which may not be available at the time that
/// `cost_and_placement` was called). This is rarely necessary and its preferred
/// to use cost_and_placement to assign uncollapsible edges ∞ cost if possible.
/// 
/// `post_collapse` This function is called after `collapse_edge` is attempted.
/// Since this collapse may have failed (e.g., it would create a non-manifold
/// mesh). This callback is provided a flag whether the attempted collapse
/// actually occurred. This callback is an opportunity for callers to update any
/// data-structures/book-keeping to acknowledge the successful (or failed)
/// collapse.
/// 
/// `stopping_condition` This function is called after a successful call to
/// `collapse_edge`. If this function returns true then the entire
/// queue-processing ends (e.g., if the number of remaining faces is below a
/// user’s threshold).
///
/// \see
///   collapse_least_cost_edge
///   collapse_edge
///   qslim

namespace igl
{
  /// Assumes (V,F) is a manifold mesh (possibly with boundary) collapses edges
  /// until desired number of faces is achieved. This uses default edge cost and
  /// merged vertex placement functions {edge length, edge midpoint}.
  ///
  /// See \fileinfo for more details.
  ///
  /// @param[in] V  #V by dim list of vertex positions
  /// @param[in] F  #F by 3 list of face indices into V.
  /// @param[in] max_m  desired number of output faces
  /// @param[in] block_intersections  whether to block intersections (see
  ///   intersection_blocking_collapse_edge_callbacks)
  /// @param[out] U  #U by dim list of output vertex posistions (can be same ref as V)
  /// @param[out] G  #G by 3 list of output face indices into U (can be same ref as G)
  /// @param[out] J  #G list of indices into F of birth face
  /// @param[out] I  #U list of indices into V of birth vertices
  /// @return true if m was reached (otherwise #G > m)
  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const int max_m,
    const bool block_intersections,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);

  /// Collapses edges of a **closed manifold mesh** (V,F) using user defined
  /// callbacks in a priority queue. Functions control the cost and placement of each collapse the
  /// stopping criteria for queue processing and the callbacks for pre and post
  /// collapse operations. See the first implementation in decimate.cpp for an
  /// example of how to deal with open/non-manifold meshes and how to adjust
  /// cost and placement functions accordingly.
  ///
  /// See \fileinfo for more details.
  ///
  /// @param[in] V  #V by dim list of vertex positions
  /// @param[in] F  #F by 3 list of face indices into V.
  /// @param[in] cost_and_placement  function computing cost of collapsing an edge and 3d
  ///     position where it should be placed:
  ///     cost_and_placement(V,F,E,EMAP,EF,EI,cost,placement);
  /// @param[in] stopping_condition  function returning whether to stop collapsing edges
  ///     based on current state. Guaranteed to be called after _successfully_
  ///     collapsing edge e removing edges (e,e1,e2) and faces (f1,f2):
  ///     bool should_stop =
  ///       stopping_condition(V,F,E,EMAP,EF,EI,Q,Qit,C,e,e1,e2,f1,f2);
  /// @param[in] pre_collapse  callback called with index of edge whose collapse is about
  ///              to be attempted (see collapse_least_cost_edge)
  /// @param[in] post_collapse  callback called with index of edge whose collapse was
  ///              just attempted and a flag revealing whether this was successful (see
  ///              collapse_least_cost_edge)
  /// @param[out] U  #U by dim list of output vertex posistions (can be same ref as V)
  /// @param[out] G  #G by 3 list of output face indices into U (can be same ref as G)
  /// @param[out] J  #G list of indices into F of birth face
  /// @param[out] I  #U list of indices into V of birth vertices
  /// @return true if m was reached (otherwise #G > m)
  ///
  /// \see connect_boundary_to_infinity
  ///
  /// \bug E,EMAP,EF,EI seem to be immediately (re)computed from F. These inputs
  /// are unused and it's not clear why.
  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const decimate_cost_and_placement_callback & cost_and_placement,
    const decimate_stopping_condition_callback & stopping_condition,
    const decimate_pre_collapse_callback       & pre_collapse,
    const decimate_post_collapse_callback      & post_collapse,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "decimate.cpp"
#endif
#endif


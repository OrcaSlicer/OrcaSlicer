// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGE_COLLAPSE_IS_VALID_H
#define IGL_EDGE_COLLAPSE_IS_VALID_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Tests whether collapsing exactly two faces and exactly 3 edges from E (e
  /// and one side of each face gets collapsed to the other) will result in a
  /// mesh with the same topology. Assumes (V,F) is a closed manifold mesh
  /// (except for previouslly collapsed faces which should be set to:
  /// [IGL_COLLAPSE_EDGE_NULL IGL_COLLAPSE_EDGE_NULL IGL_COLLAPSE_EDGE_NULL].
  ///
  /// @param[in] e  index into E of edge to try to collapse. E(e,:) = [s d] or [d s] so
  ///     that s<d, then d is collapsed to s.
  /// @param[in] F  #F by 3 list of face indices into V.
  /// @param[in] E  #E by 2 list of edge indices into V.
  /// @param[in] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[in] EI  #E by 2 list of edge flap corners (see above).
  /// @return true if edge collapse is valid
  template <
    typename DerivedF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename DerivedEI>
  IGL_INLINE bool edge_collapse_is_valid(
    const int e,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DerivedEF> & EF,
    const Eigen::MatrixBase<DerivedEI> & EI);
  /// Tests whether collapsing exactly two faces and exactly 3 edges from E (e
  /// and one side of each face gets collapsed to the other) will result in a
  /// mesh with the same topology. 
  ///
  /// @param[in] Nsv  #Nsv list of "next" vertices circulating around starting vertex of
  ///     edge
  /// @param[in] Ndv  #Ndv list of "next" vertices circulating around destination vertex of
  ///     edge
  /// @param[out] Nsv  (side-effect: sorted by value)
  /// @param[out] Ndv  (side-effect: sorted by value)
  /// @return true iff edge collapse is valid
  ///
  /// \see circulation
  IGL_INLINE bool edge_collapse_is_valid(
    /*const*/ std::vector<int> & Nsv,
    /*const*/ std::vector<int> & Ndv);
}
#ifndef IGL_STATIC_LIBRARY
#  include "edge_collapse_is_valid.cpp"
#endif
#endif

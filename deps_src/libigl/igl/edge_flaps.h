// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGE_FLAPS_H
#define IGL_EDGE_FLAPS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Determine "edge flaps": two faces on either side of a unique edge (assumes
  /// edge-manifold mesh)
  ///
  /// @param[in] F  #F by 3 list of face indices
  /// @param[in] uE  #uE by 2 list of edge indices into V.
  /// @param[in] EMAP #F*3 list of indices into uE, mapping each directed edge to unique
  ///     unique edge in uE
  /// @param[out] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[out] EI  #E by 2 list of edge flap corners (see above).
  ///
  /// \see unique_edge_map
  ///
  /// \note This seems to be a duplicate of edge_topology.h
  /// \code{cpp}
  /// igl::edge_topology(V,F,etEV,etFE,etEF);
  /// igl::edge_flaps(F,efE,efEMAP,efEF,efEI);
  /// [~,I] = sort(efE,2)
  /// all( efE(sub2ind(size(efE),repmat(1:size(efE,1),2,1)',I)) == etEV )
  /// all( efEF(sub2ind(size(efE),repmat(1:size(efE,1),2,1)',I)) == etEF )
  /// all(efEMAP(sub2ind(size(F),repmat(1:size(F,1),3,1)',repmat([1 2 3],size(F,1),1))) == etFE(:,[2 3 1]))
  /// \endcode
  template <
    typename DerivedF,
    typename DeriveduE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename DerivedEI>
  IGL_INLINE void edge_flaps(
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DeriveduE> & uE,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    Eigen::PlainObjectBase<DerivedEF> & EF,
    Eigen::PlainObjectBase<DerivedEI> & EI);
  /// \overload
  template <
    typename DerivedF,
    typename DeriveduE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename DerivedEI>
  IGL_INLINE void edge_flaps(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DeriveduE> & uE,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
    Eigen::PlainObjectBase<DerivedEF> & EF,
    Eigen::PlainObjectBase<DerivedEI> & EI);
}
#ifndef IGL_STATIC_LIBRARY
#  include "edge_flaps.cpp"
#endif

#endif

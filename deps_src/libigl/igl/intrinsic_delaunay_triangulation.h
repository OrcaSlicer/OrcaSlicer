// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_INTRINSIC_DELAUNAY_TRIANGULATION_H
#define IGL_INTRINSIC_DELAUNAY_TRIANGULATION_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// INTRINSIC_DELAUNAY_TRIANGULATION Flip edges _intrinsically_ until all are
  /// "intrinsic Delaunay". See "An algorithm for the construction of intrinsic
  /// delaunay triangulations with applications to digital geometry processing"
  /// [Fisher et al. 2007].
  ///
  /// @param[in] l_in  #F_in by 3 list of edge lengths (see edge_lengths)
  /// @param[in] F_in  #F_in by 3 list of face indices into some unspecified vertex list V
  /// @param[out] l  #F by 3 list of edge lengths
  /// @param[out] F  #F by 3 list of new face indices. Note: Combinatorially F may contain
  ///     non-manifold edges, duplicate faces and self-loops (e.g., an edge [1,1]
  ///     or a face [1,1,1]). However, the *intrinsic geometry* is still
  ///     well-defined and correct. See [Fisher et al. 2007] Figure 3 and 2nd to
  ///     last paragraph of 1st page. Since F may be "non-eddge-manifold" in the
  ///     usual combinatorial sense, it may be useful to call the more verbose
  ///     overload below if disentangling edges will be necessary later on.
  ///     Calling unique_edge_map on this F will give a _different_ result than
  ///     those outputs.
  ///
  /// \see is_intrinsic_delaunay
  template <
    typename Derivedl_in,
    typename DerivedF_in,
    typename Derivedl,
    typename DerivedF>
  IGL_INLINE void intrinsic_delaunay_triangulation(
    const Eigen::MatrixBase<Derivedl_in> & l_in,
    const Eigen::MatrixBase<DerivedF_in> & F_in,
    Eigen::PlainObjectBase<Derivedl> & l,
    Eigen::PlainObjectBase<DerivedF> & F);
  /// \overload 
  /// @param[out] E  #F*3 by 2 list of all directed edges, such that E.row(f+#F*c) is the
  /// @param[out]   edge opposite F(f,c)
  /// @param[out] uE  #uE by 2 list of unique undirected edges
  /// @param[out] EMAP #F*3 list of indices into uE, mapping each directed edge to unique
  /// @param[out]   undirected edge
  /// @param[out] uE2E  #uE list of lists of indices into E of coexisting edges
  ///
  /// \see unique_edge_map
  template <
    typename Derivedl_in,
    typename DerivedF_in,
    typename Derivedl,
    typename DerivedF,
    typename DerivedE,
    typename DeriveduE,
    typename DerivedEMAP,
    typename uE2EType>
  IGL_INLINE void intrinsic_delaunay_triangulation(
    const Eigen::MatrixBase<Derivedl_in> & l_in,
    const Eigen::MatrixBase<DerivedF_in> & F_in,
    Eigen::PlainObjectBase<Derivedl> & l,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DeriveduE> & uE,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
    std::vector<std::vector<uE2EType> > & uE2E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "intrinsic_delaunay_triangulation.cpp"
#endif

#endif

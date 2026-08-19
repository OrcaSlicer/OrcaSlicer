// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNIQUE_EDGE_MAP_H
#define IGL_UNIQUE_EDGE_MAP_H
#include "igl_inline.h"
#include <Eigen/Dense>
#include <vector>
namespace igl
{
  /// Construct relationships between facet "half"-(or rather "viewed")-edges E
  /// to unique edges of the mesh seen as a graph.
  ///
  /// @param[in] F  #F by 3  list of simplices
  /// @param[out] E  #F*3 by 2 list of all directed edges, such that E.row(f+#F*c) is the
  ///     edge opposite F(f,c)
  /// @param[out] uE  #uE by 2 list of unique undirected edges
  /// @param[out] EMAP #F*3 list of indices into uE, mapping each directed edge to unique
  ///     undirected edge so that uE(EMAP(f+#F*c)) is the unique edge
  ///     corresponding to E.row(f+#F*c)
  /// @param[out] uE2E  #uE list of lists of indices into E of coexisting edges, so that
  ///     E.row(uE2E[i][j]) corresponds to uE.row(i) for all j in
  ///     0..uE2E[i].size()-1.
  template <
    typename DerivedF,
    typename DerivedE,
    typename DeriveduE,
    typename DerivedEMAP,
    typename uE2EType>
  IGL_INLINE void unique_edge_map(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DeriveduE> & uE,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
    std::vector<std::vector<uE2EType> > & uE2E);
  /// \overload
  template <
    typename DerivedF,
    typename DerivedE,
    typename DeriveduE,
    typename DerivedEMAP>
  IGL_INLINE void unique_edge_map(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DeriveduE> & uE,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP);
  /// \overload
  /// \brief It is usual much faster if your algorithm can be written in terms of
  /// (uEC,uEE) rather than (uE2E).
  ///
  /// @param[out] uEC  #uE+1 list of cumulative counts of directed edges sharing each
  ///     unique edge so the uEC(i+1)-uEC(i) is the number of directed edges
  ///     sharing the ith unique edge.
  /// @param[out] uEE  #E list of indices into E, so that the consecutive segment of
  ///     indices uEE.segment(uEC(i),uEC(i+1)-uEC(i)) lists all directed edges
  ///     sharing the ith unique edge.
  ///
  /// #### Example:
  ///
  /// \code{cpp}
  /// // Using uE2E
  /// for(int u = 0;u<uE2E.size();u++)
  /// {
  ///   for(int i = 0;i<uE2E[u].size();i++)
  ///   {
  ///     // eth directed-edge is ith edge equivalent to uth undirected edge
  ///     e = uE2E[u][i]; 
  ///   }
  /// }
  /// \endcode
  /// 
  /// \code{cpp}
  /// // Using uEC,uEE
  /// for(int u = 0;u<uE.rows();u++)
  /// {
  ///   for(int j = uEC(u);j<uEC(u+1);j++)
  ///   {
  ///     e = uEE(j); // i = j-uEC(u);
  ///   }
  /// }
  /// \endcode
  ///
  template <
    typename DerivedF,
    typename DerivedE,
    typename DeriveduE,
    typename DerivedEMAP,
    typename DeriveduEC,
    typename DeriveduEE>
  IGL_INLINE void unique_edge_map(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DeriveduE> & uE,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
    Eigen::PlainObjectBase<DeriveduEC> & uEC,
    Eigen::PlainObjectBase<DeriveduEE> & uEE);

}
#ifndef IGL_STATIC_LIBRARY
#  include "unique_edge_map.cpp"
#endif

#endif

#ifndef IGL_SHARP_EDGES_H
#define IGL_SHARP_EDGES_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Given a mesh, compute sharp edges.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle mesh indices into V
  /// @param[in] angle  dihedral angle considered to sharp (e.g., igl::PI * 0.11)
  /// @param[out] SE  #SE by 2 list of edge indices into V
  /// @param[out] uE  #uE by 2 list of unique undirected edges
  /// @param[out] EMAP #F*3 list of indices into uE, mapping each directed edge to unique
  ///     undirected edge so that uE(EMAP(f+#F*c)) is the unique edge
  ///     corresponding to E.row(f+#F*c)
  /// @param[out] uE2E  #uE list of lists of indices into E of coexisting edges, so that
  ///     E.row(uE2E[i][j]) corresponds to uE.row(i) for all j in
  ///     0..uE2E[i].size()-1.
  /// @param[out] sharp  #SE list of indices into uE revealing sharp undirected edges
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedSE,
    typename DerivedE,
    typename DeriveduE,
    typename DerivedEMAP,
    typename uE2Etype,
    typename sharptype>
  IGL_INLINE void sharp_edges(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const typename DerivedV::Scalar angle,
    Eigen::PlainObjectBase<DerivedSE> & SE,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DeriveduE> & uE,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
    std::vector<std::vector<uE2Etype> > & uE2E,
    std::vector< sharptype > & sharp);
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedSE>
  IGL_INLINE void sharp_edges(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const typename DerivedV::Scalar angle,
    Eigen::PlainObjectBase<DerivedSE> & SE
    );
}

#ifndef IGL_STATIC_LIBRARY
#  include "sharp_edges.cpp"
#endif

#endif 

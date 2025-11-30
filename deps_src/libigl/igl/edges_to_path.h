#ifndef IGL_EDGES_TO_PATH_H
#define IGL_EDGES_TO_PATH_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Given a set of undirected, unique edges such that all form a
  /// single connected compoent with exactly 0 or 2 nodes with valence =1,
  /// determine the/a path visiting all nodes.
  ///
  /// @param[in] E  #E by 2 list of undirected edges
  /// @param[out] I  #E+1 list of nodes in order tracing the chain (loop), if the output
  ///     is a loop then I(1) == I(end)
  /// @param[out] J  #I-1 list of indices into E of edges tracing I
  /// @param[out] K  #I-1 list of indices into columns of E {0,1} so that K(i) means that
  ///     E(i,K(i)) comes before the other (i.e., E(i,3-K(i)) ). This means that 
  ///     I(i) == E(J(i),K(i)) for i<#I, or
  ///     I == E(sub2ind(size(E),J([1:end end]),[K;3-K(end)]))))
  /// 
  template <
    typename DerivedE,
    typename DerivedI,
    typename DerivedJ,
    typename DerivedK>
  IGL_INLINE void edges_to_path(
    const Eigen::MatrixBase<DerivedE> & E,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedJ> & J,
    Eigen::PlainObjectBase<DerivedK> & K);
}
#ifndef IGL_STATIC_LIBRARY
#  include "edges_to_path.cpp"
#endif
#endif

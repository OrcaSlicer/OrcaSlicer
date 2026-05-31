#ifndef IGL_QUAD_EDGES_H
#define IGL_QUAD_EDGES_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Compute the edges of a quad mesh.
  ///
  /// @param[in] Q  #Q by 4 list of quad indices into rows of some vertex list V
  /// @param[out] E  #E by 2 list of edge indices into rows of V
  template <
    typename DerivedQ,
    typename DerivedE >
  IGL_INLINE void quad_edges(
    const Eigen::MatrixBase<DerivedQ> & Q,
    Eigen::PlainObjectBase<DerivedE> & E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "quad_edges.cpp"
#endif

#endif


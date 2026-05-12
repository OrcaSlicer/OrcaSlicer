#ifndef IGL_ICOSAHEDRON_H
#define IGL_ICOSAHEDRON_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Construct a icosahedron with radius 1 centered at the origin
  ///
  /// @param[out] V  #V by 3 list of vertex positions
  /// @param[out] F  #F by 3 list of triangle indices into rows of V
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE void icosahedron(
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "icosahedron.cpp"
#endif

#endif

